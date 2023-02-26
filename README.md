# VK_RAYTRACE
![vk_raytrace](doc/vk_raytrace.png)


This project is a [glTF 2.0](https://www.khronos.org/gltf/) sample viewer using [Vulkan ray tracing](https://www.khronos.org/blog/vulkan-ray-tracing-final-specification-release). It follows the [ray tracing tutorial](https://github.com/nvpro-samples/vk_raytracing_tutorial_KHR) and combines all chapters into a single example. 

The lighting equation is based on:
* [glTF PBR reference implementation](https://github.com/KhronosGroup/glTF-WebGL-PBR) from Khronos. 
* [Disney PBR](https://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_slides_v2.pdf)

The rendering pipeline can be switched from:
* **RTX**: RayGen, Closest-Hit, Miss, Any-Hit model
* **Compute**: using Ray Query



# Rendering engine

## Framework: NAME
Logging: simple thread based logging --> initially header based
threading: add gated threads
Communication: 
Profiling: Wrapper for tracy

## Rendering: 
Modular --> add multiple renderers with different config (shaders/camera types (pinhole/ftheta/depth)
Denoising


## future work
* add denoising
* split rendering and UI into separate threads, fix rendering thread rate @30Hz (configurable)
* add headless mode
* add osi receiver