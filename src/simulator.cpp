/*
 * Copyright (c) 2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Main class to render the scene, holds sub-classes for various work
 */

#define VMA_IMPLEMENTATION

#include "simulator.hpp"

#include "denoiser.hpp"
#include "fileformats/tiny_gltf_freeimage.h"
#include "gui.hpp"
#include "nvml_monitor.hpp"
#include "rayquery.hpp"
#include "rtx_pipeline.hpp"
#include "shaders/host_device.h"
#include "tools.hpp"
#include "tracy/Tracy.hpp"

#if defined(NVP_SUPPORTS_NVML)
NvmlMonitor g_nvml(100, 100);
#endif

//--------------------------------------------------------------------------------------------------
// Keep the handle on the device
// Initialize the tool to do all our allocations: buffers, images
//
void Simulator::setup(const VkInstance& instance, const VkDevice& device, const VkPhysicalDevice& physicalDevice,
                      const std::vector<nvvk::Queue>& queues) {
  AppBase::setup(instance, device, physicalDevice, queues[eGCT0].familyIndex);

  m_gui = std::make_shared<SimGUI>(this);  // GUI of this class

  // Memory allocator for buffers and images
  m_alloc.init(instance, device, physicalDevice);

  m_debug.setup(m_device);

  // Compute queues can be use for acceleration structures
  m_picker.setup(m_device, physicalDevice, queues[eCompute].familyIndex, &m_alloc);
  m_accelStruct.setup(m_device, physicalDevice, queues[eCompute].familyIndex, &m_alloc);

  // Note: the GTC family queue is used because the nvvk::cmdGenerateMipmaps uses vkCmdBlitImage and this
  // command requires graphic queue and not only transfer.
  m_scene.setup(m_device, physicalDevice, queues[eGCT1], &m_alloc);

  // Transfer queues can be use for the creation of the following assets
  m_offscreen.setup(m_device, physicalDevice, queues[eTransfer].familyIndex, &m_alloc);
  m_skydome.setup(device, physicalDevice, queues[eTransfer].familyIndex, &m_alloc);

  // Create and setup all renderers
  m_pRender[eRtxPipeline] = new RtxPipeline;
  m_pRender[eRayQuery]    = new RayQuery;
  for (auto r : m_pRender) {
    r->setup(m_device, physicalDevice, queues[eTransfer].familyIndex, &m_alloc);
  }
}

//--------------------------------------------------------------------------------------------------
// Loading the scene file, setting up all scene buffers, create the acceleration structures
// for the loaded models.
//
void Simulator::loadScene(const std::string& filename) {
  m_scene.load(filename);
  m_accelStruct.create(m_scene.getScene(), m_scene.getBuffers(Scene::eVertex), m_scene.getBuffers(Scene::eIndex));

  // The picker is the helper to return information from a ray hit under the mouse cursor
  m_picker.setTlas(m_accelStruct.getTlas());
  resetFrame();
}

//--------------------------------------------------------------------------------------------------
// Loading an HDR image and creating the importance sampling acceleration structure
//
void Simulator::loadEnvironmentHdr(const std::string& hdrFilename) {
  MilliTimer timer;
  LOGI("Loading HDR and converting %s\n", hdrFilename.c_str());
  m_skydome.loadEnvironment(hdrFilename);
  timer.print();

  m_rtxState.fireflyClampThreshold = m_skydome.getIntegral() * 4.f;  // magic
}

//--------------------------------------------------------------------------------------------------
// Loading asset in a separate thread
// - Used by file drop and menu operation
// Marking the session as busy, to avoid calling rendering while loading assets
//
void Simulator::loadAssets(const char* filename) {
  std::string sfile = filename;

  // Need to stop current rendering
  m_busy = true;
  vkDeviceWaitIdle(m_device);

  std::thread([&, sfile]() {
    LOGI("Loading: %s\n", sfile.c_str());

    // Supporting only GLTF and HDR files
    namespace fs          = std::filesystem;
    std::string extension = fs::path(sfile).extension().string();
    if (extension == ".gltf" || extension == ".glb") {
      m_busyReasonText = "Loading scene ";

      // Loading scene and creating acceleration structure
      loadScene(sfile);

      // Loading the scene might have loaded new textures, which is changing the number of elements
      // in the DescriptorSetLayout. Therefore, the PipelineLayout will be out-of-date and need
      // to be re-created. If they are re-created, the pipeline also need to be re-created.
      for (auto& r : m_pRender) r->destroy();

      m_pRender[m_rndMethod]->create(
        m_size, {m_accelStruct.getDescLayout(), m_offscreen.getDescLayout(), m_scene.getDescLayout(), m_descSetLayout},
        &m_scene);
    }

    if (extension == ".hdr")  //|| extension == ".exr")
    {
      m_busyReasonText = "Loading HDR ";
      loadEnvironmentHdr(sfile);
      updateHdrDescriptors();
    }

    // Re-starting the frame count to 0
    Simulator::resetFrame();
    m_busy = false;
  }).detach();
}

//--------------------------------------------------------------------------------------------------
// Called at each frame to update the UBO: scene, camera, environment (sun&sky)
//
void Simulator::updateUniformBuffer(const VkCommandBuffer& cmdBuf) {
  if (m_busy)
    return;

  LABEL_SCOPE_VK(cmdBuf);
  const float aspectRatio = m_renderRegion.extent.width / static_cast<float>(m_renderRegion.extent.height);

  m_scene.updateCamera(cmdBuf, aspectRatio);
  vkCmdUpdateBuffer(cmdBuf, m_sunAndSkyBuffer.buffer, 0, sizeof(SunAndSky), &m_sunAndSky);
}

//--------------------------------------------------------------------------------------------------
// If the camera matrix has changed, resets the frame otherwise, increments frame.
//
void Simulator::updateFrame() {
  static nvmath::mat4f refCamMatrix;
  static float         fov = 0;

  auto& m = CameraManip.getMatrix();
  auto  f = CameraManip.getFov();
  if (memcmp(&refCamMatrix.a00, &m.a00, sizeof(nvmath::mat4f)) != 0 || f != fov) {
    resetFrame();
    refCamMatrix = m;
    fov          = f;
  }

  if (m_rtxState.frame < m_maxFrames)
    m_rtxState.frame++;
}

//--------------------------------------------------------------------------------------------------
// Reset frame is re-starting the rendering
//
void Simulator::resetFrame() {
  m_rtxState.frame = -1;
}

//--------------------------------------------------------------------------------------------------
// Descriptors for the Sun&Sky buffer
//
void Simulator::createDescriptorSetLayout() {
  VkShaderStageFlags flags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                             VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT;

  m_bind.addBinding({EnvBindings::eSunSky, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_MISS_BIT_KHR | flags});
  m_bind.addBinding({EnvBindings::eHdr, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, flags});  // HDR image
  m_bind.addBinding({EnvBindings::eImpSamples, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags});   // importance sampling

  m_descPool = m_bind.createPool(m_device, 1);
  CREATE_NAMED_VK(m_descSetLayout, m_bind.createLayout(m_device));
  CREATE_NAMED_VK(m_descSet, nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout));

  // Using the environment
  std::vector<VkWriteDescriptorSet> writes;
  VkDescriptorBufferInfo            sunskyDesc{m_sunAndSkyBuffer.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo            accelImpSmpl{m_skydome.m_accelImpSmpl.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eSunSky, &sunskyDesc));
  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eHdr, &m_skydome.m_texHdr.descriptor));
  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eImpSamples, &accelImpSmpl));

  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Setting the descriptor for the HDR and its acceleration structure
//
void Simulator::updateHdrDescriptors() {
  std::vector<VkWriteDescriptorSet> writes;
  VkDescriptorBufferInfo            accelImpSmpl{m_skydome.m_accelImpSmpl.buffer, 0, VK_WHOLE_SIZE};

  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eHdr, &m_skydome.m_texHdr.descriptor));
  writes.emplace_back(m_bind.makeWrite(m_descSet, EnvBindings::eImpSamples, &accelImpSmpl));
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Creating the uniform buffer holding the Sun&Sky structure
// - Buffer is host visible and will be set each frame
//
void Simulator::createUniformBuffer() {
  m_sunAndSkyBuffer =
    m_alloc.createBuffer(sizeof(SunAndSky), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  NAME_VK(m_sunAndSkyBuffer.buffer);
}

//--------------------------------------------------------------------------------------------------
// Destroying all allocations
//
void Simulator::destroyResources() {
  // Resources
  m_alloc.destroy(m_sunAndSkyBuffer);

  // Descriptors
  vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);

  // Other
  m_picker.destroy();
  m_scene.destroy();
  m_accelStruct.destroy();
  m_offscreen.destroy();
  m_skydome.destroy();
  m_axis.deinit();

  // All renderers
  for (auto p : m_pRender) {
    p->destroy();
    p = nullptr;
  }
  // #ifdef NVP_SUPPORTS_OPTIX7
  //   m_denoiser->destroy();
  // #endif

  // Memory
  m_alloc.deinit();
}

//--------------------------------------------------------------------------------------------------
// Handling resize of the window
//
void Simulator::onResize(int /*w*/, int /*h*/) {
  m_offscreen.update(m_size);
  resetFrame();
}

//--------------------------------------------------------------------------------------------------
// Call the rendering of all graphical user interface
//
void Simulator::renderGui(nvvk::ProfilerVK& profiler) {
  ZoneScopedN("renderGui");

  m_gui->titleBar();
  m_gui->render(profiler);
  m_gui->menuBar();

  auto& IO = ImGui::GetIO();
  if (ImGui::IsMouseDoubleClicked(ImGuiDir_Left) && !ImGui::GetIO().WantCaptureKeyboard) {
    screenPicking();
  }
}

//--------------------------------------------------------------------------------------------------
// Creating the render: RTX, Ray Query, ...
// - Destroy the previous one.
void Simulator::createRender(RndMethod method) {
  if (method == m_rndMethod)
    return;

  LOGI("Switching renderer, from %d to %d \n", m_rndMethod, method);
  if (m_rndMethod != eNone) {
    vkDeviceWaitIdle(m_device);  // cannot destroy while in use
    m_pRender[m_rndMethod]->destroy();
  }
  m_rndMethod = method;

  m_pRender[m_rndMethod]->create(
    m_size, {m_accelStruct.getDescLayout(), m_offscreen.getDescLayout(), m_scene.getDescLayout(), m_descSetLayout},
    &m_scene);
}

//--------------------------------------------------------------------------------------------------
// The GUI is taking space and size of the rendering area is smaller than the viewport
// This is the space left in the center view.
void Simulator::setRenderRegion(const VkRect2D& size) {
  if (memcmp(&m_renderRegion, &size, sizeof(VkRect2D)) != 0)
    resetFrame();
  m_renderRegion = size;
}

//////////////////////////////////////////////////////////////////////////
// Post ray tracing
//////////////////////////////////////////////////////////////////////////

void Simulator::createOffscreenRender() {
  m_offscreen.create(m_size, m_renderPass);
  m_axis.init(m_device, m_renderPass, 0, 50.0f);
}

//--------------------------------------------------------------------------------------------------
// This will draw the result of the rendering and apply the tonemapper.
// If enabled, draw orientation axis in the lower left corner.
// POSTPROCESSING
void Simulator::drawPost(VkCommandBuffer cmdBuf) {
  LABEL_SCOPE_VK(cmdBuf);
  auto size = nvmath::vec2f(m_size.width, m_size.height);
  auto area = nvmath::vec2f(m_renderRegion.extent.width, m_renderRegion.extent.height);
  // SOFINDE VIEWPORT
  VkViewport viewport{static_cast<float>(m_renderRegion.offset.x),
                      static_cast<float>(m_renderRegion.offset.y),
                      static_cast<float>(m_size.width),
                      static_cast<float>(m_size.height),
                      0.0f,
                      1.0f};
  VkRect2D   scissor{m_renderRegion.offset, {m_renderRegion.extent.width, m_renderRegion.extent.height}};
  vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
  vkCmdSetScissor(cmdBuf, 0, 1, &scissor);

  m_offscreen.m_tonemapper.zoom           = m_descaling ? 1.0f / m_descalingLevel : 1.0f;
  m_offscreen.m_tonemapper.renderingRatio = size / area;
  m_offscreen.run(cmdBuf);

  if (m_showAxis)
    m_axis.display(cmdBuf, CameraManip.getMatrix(), m_size);
}

// // #OPTIX_D
// // Will copy the Vulkan images to Cuda buffers
// void copyImagesToCuda(VkCommandBuffer cmd) {
// #ifdef NVP_SUPPORTS_OPTIX7
//   nvvk::Texture result{m_gBuffers->getColorImage(eGBufResult), nullptr,
//                        m_gBuffers->getDescriptorImageInfo(eGBufResult)};
//   nvvk::Texture albedo{m_gBuffers->getColorImage(eGBufAlbedo), nullptr,
//                        m_gBuffers->getDescriptorImageInfo(eGBufAlbedo)};
//   nvvk::Texture normal{m_gBuffers->getColorImage(eGBufNormal), nullptr,
//                        m_gBuffers->getDescriptorImageInfo(eGBufNormal)};
//   m_denoiser->imageToBuffer(cmd, {result, albedo, normal});
// #endif  // NVP_SUPPORTS_OPTIX7
// }

// // #OPTIX_D
// // Copy the denoised buffer to Vulkan image
// void copyCudaImagesToVulkan(VkCommandBuffer cmd) {
// #ifdef NVP_SUPPORTS_OPTIX7
//   nvvk::Texture denoised{m_gBuffers->getColorImage(eGbufDenoised), nullptr,
//                          m_gBuffers->getDescriptorImageInfo(eGbufDenoised)};
//   m_denoiser->bufferToImage(cmd, &denoised);
// #endif  // NVP_SUPPORTS_OPTIX7
// }

// // #OPTIX_D
// // Invoke the Optix denoiser
// void denoiseImage() {
// #ifdef NVP_SUPPORTS_OPTIX7
//   m_denoiser->denoiseImageBuffer(m_fenceValue);
// #endif  // NVP_SUPPORTS_OPTIX7
// }

// // #OPTIX_D
// // Determine which image will be displayed, the original from ray tracer or the denoised one
// bool showDenoisedImage() const {
//   return m_settings.denoiseApply && ((m_frame >= m_settings.denoiseEveryNFrames) || m_settings.denoiseFirstFrame ||
//                                      (m_frame >= m_settings.maxFrames));
// }

// // We are submitting the command buffer using a timeline semaphore, semaphore used by Cuda to wait
// // for the Vulkan execution before denoising
// // #OPTIX_D
// void submitSignalSemaphore(const VkCommandBuffer& cmdBuf) {
//   VkCommandBufferSubmitInfoKHR cmd_buf_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR};
//   cmd_buf_info.commandBuffer = cmdBuf;

// #ifdef NVP_SUPPORTS_OPTIX7
//   // Increment for signaling
//   m_fenceValue++;

//   VkSemaphoreSubmitInfoKHR signal_semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR};
//   signal_semaphore.semaphore = m_denoiser->getTLSemaphore();
//   signal_semaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
//   signal_semaphore.value     = m_fenceValue;
// #endif  // NVP_SUPPORTS_OPTIX7

//   VkSubmitInfo2KHR submits{VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR};
//   submits.commandBufferInfoCount = 1;
//   submits.pCommandBufferInfos    = &cmd_buf_info;
// #ifdef NVP_SUPPORTS_OPTIX7
//   submits.signalSemaphoreInfoCount = 1;
//   submits.pSignalSemaphoreInfos    = &signal_semaphore;
// #endif  // _DEBUG

//   vkQueueSubmit2(queue, 1, &submits, {});
// }

void Simulator::renderScene(const VkCommandBuffer& cmdBuf, nvvk::ProfilerVK& profiler) {
#if defined(NVP_SUPPORTS_NVML)
  g_nvml.refresh();
#endif

  if (m_busy) {
    m_gui->showBusyWindow();  // Busy while loading scene
    return;
  }

  LABEL_SCOPE_VK(cmdBuf);

  auto sec = profiler.timeRecurring("Render", cmdBuf);

  // We are done rendering
  if (m_rtxState.frame >= m_maxFrames)
    return;

  // Handling de-scaling by reducing the size to render
  VkExtent2D render_size = m_renderRegion.extent;
  if (m_descaling)
    render_size = VkExtent2D{render_size.width / m_descalingLevel, render_size.height / m_descalingLevel};

  m_rtxState.size = {render_size.width, render_size.height};
  // m_rtxState.size = {1920, 1080};
  // State is the push constant structure
  m_pRender[m_rndMethod]->setPushContants(m_rtxState);
  // Running the renderer
  m_pRender[m_rndMethod]->run(cmdBuf, render_size, profiler,
                              {m_accelStruct.getDescSet(), m_offscreen.getDescSet(), m_scene.getDescSet(), m_descSet});

  //   // denoising
  // #ifdef NVP_SUPPORTS_OPTIX7
  //   // #OPTIX_D
  //   if (needToDenoise()) {
  //     // Submit raytracing and signal
  //     copyImagesToCuda(cmd);
  //     vkEndCommandBuffer(cmd);
  //     // submitSignalSemaphore(cmd);

  //     // #OPTIX_D
  //     // Denoiser waits for signal and submit new one when done
  //     denoiseImage();

  //     // // #OPTIX_D
  //     // // Adding a wait semaphore to the application, such that the frame command buffer,
  //     // // will wait for the end of the denoised image before executing the command buffer.
  //     // VkSemaphoreSubmitInfoKHR wait_semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR};
  //     // wait_semaphore.semaphore = m_denoiser->getTLSemaphore();
  //     // wait_semaphore.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  //     // wait_semaphore.value     = m_fenceValue;
  //     // m_app->addWaitSemaphore(wait_semaphore);

  //     // #OPTIX_D
  //     // Continue rendering pipeline (renewing the command buffer)
  //     VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  //     begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  //     vkBeginCommandBuffer(cmd, &begin_info);
  //     copyCudaImagesToVulkan(cmd);
  //   }
  // #endif

  // For automatic brightness tonemapping
  if (m_offscreen.m_tonemapper.autoExposure) {
    auto slot = profiler.timeRecurring("Mipmap", cmdBuf);
    m_offscreen.genMipmap(cmdBuf);
  }
}

//////////////////////////////////////////////////////////////////////////
// Keyboard / Drag and Drop
//////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
// Overload keyboard hit
// - Home key: fit all, the camera will move to see the entire scene bounding box
// - Space: Trigger ray picking and set the interest point at the intersection
//          also return all information under the cursor
//
void Simulator::onKeyboard(int key, int scancode, int action, int mods) {
  AppBase::onKeyboard(key, scancode, action, mods);

  if (m_busy || action == GLFW_RELEASE)
    return;

  switch (key) {
    case GLFW_KEY_HOME:
    case GLFW_KEY_F:  // Set the camera as to see the model
      fitCamera(m_scene.getScene().m_dimensions.min, m_scene.getScene().m_dimensions.max, false);
      break;
    case GLFW_KEY_SPACE:
      screenPicking();
      break;
    case GLFW_KEY_R:
      resetFrame();
      break;
    default:
      break;
  }
}

//--------------------------------------------------------------------------------------------------
//
//
void Simulator::screenPicking() {
  double x, y;
  glfwGetCursorPos(m_window, &x, &y);

  // Set the camera as to see the model
  nvvk::CommandPool sc(m_device, m_graphicsQueueIndex);
  VkCommandBuffer   cmdBuf = sc.createCommandBuffer();

  const float aspectRatio = m_renderRegion.extent.width / static_cast<float>(m_renderRegion.extent.height);
  const auto& view        = CameraManip.getMatrix();
  auto        proj        = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.1f, 1000.0f);

  nvvk::RayPickerKHR::PickInfo pickInfo;
  pickInfo.pickX          = float(x - m_renderRegion.offset.x) / float(m_renderRegion.extent.width);
  pickInfo.pickY          = float(y - m_renderRegion.offset.y) / float(m_renderRegion.extent.height);
  pickInfo.modelViewInv   = nvmath::invert(view);
  pickInfo.perspectiveInv = nvmath::invert(proj);

  m_picker.run(cmdBuf, pickInfo);
  sc.submitAndWait(cmdBuf);

  nvvk::RayPickerKHR::PickResult pr = m_picker.getResult();

  if (pr.instanceID == ~0) {
    LOGI("Nothing Hit\n");
    return;
  }

  nvmath::vec3f worldPos = nvmath::vec3f(pr.worldRayOrigin + pr.worldRayDirection * pr.hitT);
  // Set the interest position
  nvmath::vec3f eye, center, up;
  CameraManip.getLookat(eye, center, up);
  CameraManip.setLookat(eye, worldPos, up, false);

  auto& prim = m_scene.getScene().m_primMeshes[pr.instanceCustomIndex];
  LOGI("Hit(%d): %s\n", pr.instanceCustomIndex, prim.name.c_str());
  LOGI(" - PrimId(%d)\n", pr.primitiveID);
}

//--------------------------------------------------------------------------------------------------
//
//
void Simulator::onFileDrop(const char* filename) {
  if (m_busy)
    return;

  loadAssets(filename);
}

//--------------------------------------------------------------------------------------------------
// Window callback when the mouse move
// - Handling ImGui and a default camera
//
void Simulator::onMouseMotion(int x, int y) {
  AppBase::onMouseMotion(x, y);
  if (m_busy)
    return;

  if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard)
    return;

  if (m_inputs.lmb || m_inputs.rmb || m_inputs.mmb) {
    m_descaling = true;
  }
}

//--------------------------------------------------------------------------------------------------
//
//
void Simulator::onMouseButton(int button, int action, int mods) {
  AppBase::onMouseButton(button, action, mods);
  if (m_busy)
    return;

  if ((m_inputs.lmb || m_inputs.rmb || m_inputs.mmb) == false && action == GLFW_RELEASE && m_descaling == true) {
    m_descaling = false;
    resetFrame();
  }
}
