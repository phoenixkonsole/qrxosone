cmake_minimum_required(VERSION 3.16)
project(qrx68100_foundation C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_library(qrx68100_core STATIC src/interpreter.c src/jit_a64.c src/virt.c src/media.c src/gpu2d.c src/glide3d.c src/accel.c src/gl_compat.c src/bench_scene.c src/qrx86_gpu.c src/qrx86_ipc.c src/qrx86_input.c src/media_demo.c src/cpu_vm.c src/boot_monitor.c)
target_include_directories(qrx68100_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(qrx68100_core PUBLIC Threads::Threads)
target_compile_options(qrx68100_core PRIVATE -Wall -Wextra -Wpedantic)
if(NOT APPLE)
  target_link_libraries(qrx68100_core PUBLIC m)
endif()
add_executable(qrx68100_smoke src/main.c)
target_link_libraries(qrx68100_smoke PRIVATE qrx68100_core)
add_executable(qrx68100_vm src/virt_main.c)
target_link_libraries(qrx68100_vm PRIVATE qrx68100_core)
add_executable(qrx68100_tests tests/vm_tests.c)
target_link_libraries(qrx68100_tests PRIVATE qrx68100_core)
target_compile_options(qrx68100_tests PRIVATE -Wall -Wextra -Wpedantic)
enable_testing()
add_test(NAME foundation_smoke COMMAND qrx68100_smoke)
add_test(NAME vm_unit_tests COMMAND qrx68100_tests)
add_test(NAME virtual_boot_monitor COMMAND qrx68100_vm)

add_executable(qrx_gpu_bridge_tests tests/gpu_bridge_tests.c)
target_link_libraries(qrx_gpu_bridge_tests PRIVATE qrx68100_core)
add_test(NAME guest_gpu_bridge_and_pcm_dma COMMAND qrx_gpu_bridge_tests)
add_executable(qrx_glide3d_tests tests/glide3d_tests.c)
target_link_libraries(qrx_glide3d_tests PRIVATE qrx68100_core)
add_test(NAME guest_glide3d_triangle_texture COMMAND qrx_glide3d_tests)
add_executable(qrx_media_tests tests/media_tests.c)
target_link_libraries(qrx_media_tests PRIVATE qrx68100_core)
add_test(NAME mmio_media_devices COMMAND qrx_media_tests)

if(APPLE)
  enable_language(OBJC)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
  find_library(AUDIO_FRAMEWORK AudioToolbox REQUIRED)
  add_executable(armithlon_native macos/armithlon_mac.m macos/qrx_metal_renderer.m)
  target_include_directories(armithlon_native PRIVATE include)
  target_link_libraries(armithlon_native PRIVATE qrx68100_core
    ${COCOA_FRAMEWORK} ${METAL_FRAMEWORK} ${METALKIT_FRAMEWORK} ${AUDIO_FRAMEWORK})
  target_compile_options(armithlon_native PRIVATE -Wall -Wextra -fobjc-arc)
  add_executable(qrx_bench_metal macos/bench_metal.m)
  target_link_libraries(qrx_bench_metal PRIVATE qrx68100_core ${METAL_FRAMEWORK} ${COCOA_FRAMEWORK})
  target_compile_options(qrx_bench_metal PRIVATE -Wall -Wextra -fobjc-arc)
endif()

add_executable(qrx68100_media_demo src/media_demo_main.c)
target_link_libraries(qrx68100_media_demo PRIVATE qrx68100_core)
if(NOT APPLE)
  target_link_libraries(qrx68100_media_demo PRIVATE m)
endif()
add_test(NAME native_media_68k_demo COMMAND qrx68100_media_demo)

# Regression tests use assert() for actual checks. Do not let Release/RelWithDebInfo
# silently disable those checks via -DNDEBUG.
foreach(test_target qrx68100_tests qrx_media_tests qrx_gpu_bridge_tests qrx_glide3d_tests)
  if(MSVC)
    target_compile_options(${test_target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${test_target} PRIVATE -UNDEBUG)
  endif()
endforeach()

# Vulkan loader probe: actual VkInstance/VkDevice creation, without requiring
# SDK headers at build time. NOT a Vulkan graphics backend yet.
add_executable(qrx_vulkan_probe src/vulkan_probe.c)
if(CMAKE_DL_LIBS)
  target_link_libraries(qrx_vulkan_probe PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(qrx_accel_tests tests/accel_tests.c)
target_link_libraries(qrx_accel_tests PRIVATE qrx68100_core)
target_compile_options(qrx_accel_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME native_metal_command_stream_and_qrx86_surface COMMAND qrx_accel_tests)
add_executable(qrx86_ipc_tests tests/qrx86_ipc_tests.c)
target_link_libraries(qrx86_ipc_tests PRIVATE qrx68100_core)
target_compile_options(qrx86_ipc_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME qrx86_hle_guest_frame_host_ipc COMMAND qrx86_ipc_tests)
add_executable(qrx86_frame_demo src/qrx86_frame_demo.c)
target_link_libraries(qrx86_frame_demo PRIVATE qrx68100_core)

add_executable(qrx86_input_tests tests/qrx86_input_tests.c)
target_link_libraries(qrx86_input_tests PRIVATE qrx68100_core)
target_compile_options(qrx86_input_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME qrx86_host_interactive_input_protocol COMMAND qrx86_input_tests)

# Optional actual Vulkan GPU transfer renderer: 2D offscreen clear/copy/image
# with GPU readback. Requires the official SDK and an installed Vulkan ICD.
# A device probe alone is NOT counted as accelerated rendering.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
  add_executable(qrx_vulkan_offscreen src/vulkan_offscreen.c)
  target_link_libraries(qrx_vulkan_offscreen PRIVATE qrx68100_core Vulkan::Vulkan)
  target_compile_options(qrx_vulkan_offscreen PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ARMITHLON 0.9: guest OpenGL immediate-mode -> native GPU command stream.
add_executable(qrx_gl_compat_tests tests/gl_compat_tests.c)
target_link_libraries(qrx_gl_compat_tests PRIVATE qrx68100_core)
target_compile_options(qrx_gl_compat_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME qrx86_opengl_subset_to_gpu_commands COMMAND qrx_gl_compat_tests)
add_executable(qrx86_command_ipc_tests tests/qrx86_command_ipc_tests.c)
target_link_libraries(qrx86_command_ipc_tests PRIVATE qrx68100_core)
target_compile_options(qrx86_command_ipc_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME qrx86_opengl_command_ipc_to_metal_stream COMMAND qrx86_command_ipc_tests)
# Optional real Vulkan graphics pipeline. The offline developer kit includes GLSL;
# glslangValidator compiles immutable SPIR-V at build time, not guest shaders.
find_program(QRX_GLSLANG_VALIDATOR NAMES glslangValidator)
if(Vulkan_FOUND AND QRX_GLSLANG_VALIDATOR)
  set(QRX_VERT_SPV "${CMAKE_CURRENT_BINARY_DIR}/qrx3d.vert.spv")
  set(QRX_FRAG_SPV "${CMAKE_CURRENT_BINARY_DIR}/qrx3d.frag.spv")
  add_custom_command(OUTPUT "${QRX_VERT_SPV}"
     COMMAND "${QRX_GLSLANG_VALIDATOR}" -V -S vert -o "${QRX_VERT_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/qrx3d.vert"
     DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/qrx3d.vert" VERBATIM)
  add_custom_command(OUTPUT "${QRX_FRAG_SPV}"
     COMMAND "${QRX_GLSLANG_VALIDATOR}" -V -S frag -o "${QRX_FRAG_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/qrx3d.frag"
     DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/qrx3d.frag" VERBATIM)
  add_executable(qrx_vulkan_3d src/vulkan_3d.c "${QRX_VERT_SPV}" "${QRX_FRAG_SPV}")
  target_link_libraries(qrx_vulkan_3d PRIVATE qrx68100_core Vulkan::Vulkan)
  target_compile_definitions(qrx_vulkan_3d PRIVATE QRX_VK_VERT_SPV="${QRX_VERT_SPV}" QRX_VK_FRAG_SPV="${QRX_FRAG_SPV}")
  target_compile_options(qrx_vulkan_3d PRIVATE -Wall -Wextra -Wpedantic)
else()
  message(STATUS "ARMITHLON Vulkan3D: Vulkan SDK + glslangValidator required; no fake backend built")
endif()

# ARMITHLON 1.0 original MIT benchmark scene and API-specific encoder tests.
add_executable(qrx_bench_api benchmarks/bench_api.c)
target_link_libraries(qrx_bench_api PRIVATE qrx68100_core)
add_executable(qrx_bench_scene_tests tests/bench_scene_tests.c)
target_link_libraries(qrx_bench_scene_tests PRIVATE qrx68100_core)
target_compile_options(qrx_bench_scene_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME qrx_bench_reference_scene COMMAND qrx_bench_scene_tests)
add_test(NAME qrx_bench_glide_mmio COMMAND qrx_bench_api glide-mmio --frames 2 --warmup 1)
add_test(NAME qrx_bench_opengl_hle COMMAND qrx_bench_api opengl-hle --frames 2 --warmup 1)
add_test(NAME qrx_bench_cpu_renderer COMMAND qrx_bench_api cpu --frames 2 --warmup 1)

# ARMITHLON 1.1 external benchmark adapters: hermetic tests, no GPU/SDK/network.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME external_benchmark_adapters
           COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-external-benchmarks.py")
endif()
