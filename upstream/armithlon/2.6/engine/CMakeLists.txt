cmake_minimum_required(VERSION 3.16)
project(qrx68100_foundation C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_library(qrx68100_core STATIC src/interpreter.c src/jit_a64.c src/virt.c src/media.c src/gpu2d.c src/glide3d.c src/accel.c src/gl_compat.c src/bench_scene.c src/x11_bridge.c src/qrx86_gpu.c src/qrx86_ipc.c src/qrx86_input.c src/media_demo.c src/cpu_vm.c src/boot_monitor.c src/qrx_block.c src/qrx_hostfs.c src/qrx_memory.c src/qrx_hunk.c bootshell/qrx_bootshell.c)
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
  # Native Cocoa frontend boots the same real 68k guest firmware as the CLI;
  # NOT an AROS Workbench or a full Amiga chipset emulator.
  add_executable(qrx68100_desktop_qrx macos/qrx_boot_desktop.m)
  target_include_directories(qrx68100_desktop_qrx PRIVATE include)
  target_link_libraries(qrx68100_desktop_qrx PRIVATE qrx68100_core ${COCOA_FRAMEWORK})
  target_compile_options(qrx68100_desktop_qrx PRIVATE -Wall -Wextra -fobjc-arc)
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

# ARMITHLON 1.2: native Linux-i386 X11 framebuffer -> GPU IPC/Metal bridge.
add_executable(qrx_x11_bridge_tests tests/x11_bridge_tests.c)
target_link_libraries(qrx_x11_bridge_tests PRIVATE qrx68100_core)
target_compile_options(qrx_x11_bridge_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME linux_i386_x11_to_metal_ipc COMMAND qrx_x11_bridge_tests)

# ARMITHLON 1.3: reproducible benchmark & QRX X11 patch tests.
if(Python3_Interpreter_FOUND)
  add_test(NAME qrx_x11_input_patch_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-patch-qrx86-x11-input.py")
  add_test(NAME qrx86_crossos_benchmark_classification
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx86-execution.py")
  add_test(NAME qrx_compat_boot_provenance_gate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-compat-boot.py")
endif()

# ARMITHLON 1.4: mature full-instruction Musashi backend (68000 through 68040).
# Explicit opt-in: vendor sources are fetched separately, verified and pinned.
option(QRX_ENABLE_MUSASHI "Enable the pinned external Musashi 68k CPU backend" OFF)
if(QRX_ENABLE_MUSASHI)
  set(QRX_MUSASHI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Musashi")
  if(NOT EXISTS "${QRX_MUSASHI_DIR}/m68kcpu.c" OR
     NOT EXISTS "${QRX_MUSASHI_DIR}/m68k_in.c" OR
     NOT EXISTS "${QRX_MUSASHI_DIR}/softfloat/softfloat.c")
    message(FATAL_ERROR "Musashi not fetched. Run ./armithlon full68k-setup first.")
  endif()
  add_executable(qrx_m68kmake "${QRX_MUSASHI_DIR}/m68kmake.c")
  set(QRX_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/musashi_generated")
  file(MAKE_DIRECTORY "${QRX_GENERATED_DIR}")
  add_custom_command(OUTPUT "${QRX_GENERATED_DIR}/m68kops.c" "${QRX_GENERATED_DIR}/m68kops.h"
    COMMAND "$<TARGET_FILE:qrx_m68kmake>" "${QRX_GENERATED_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/musashi_patched/m68k_in.c"
    DEPENDS qrx_m68kmake "${CMAKE_CURRENT_BINARY_DIR}/musashi_patched/m68k_in.c"
    COMMENT "Generate pin-verified Motorola 680x0 opcode tables"
    VERBATIM)
  # ARMITHLON 2.7: FSAVE-(An) IDLE-frame fix on a build-tree COPY of the pinned
  # Musashi sources (vendor tree stays pristine for fetch-musashi.sh).
  # Real 68040 FSAVE (An) stores the IDLE frame without adjusting An; upstream
  # Musashi fatalerror()s on this mode, which AROS cpu_detect.S uses to probe
  # the FPU, so the AROS ROM cannot boot without it.
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "Musashi backend needs python3 for the FSAVE build-tree patch.")
  endif()
  set(QRX_MUSASHI_PATCHED_DIR "${CMAKE_CURRENT_BINARY_DIR}/musashi_patched")
  file(MAKE_DIRECTORY "${QRX_MUSASHI_PATCHED_DIR}")
  add_custom_command(OUTPUT "${QRX_MUSASHI_PATCHED_DIR}/m68kcpu.c" "${QRX_MUSASHI_PATCHED_DIR}/m68kfpu.c" "${QRX_MUSASHI_PATCHED_DIR}/m68k_in.c"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/patch-musashi-fsave-an.py"
      "${QRX_MUSASHI_DIR}" "${QRX_MUSASHI_PATCHED_DIR}"
    DEPENDS "${QRX_MUSASHI_DIR}/m68kcpu.c" "${QRX_MUSASHI_DIR}/m68kfpu.c" "${QRX_MUSASHI_DIR}/m68k_in.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/../tools/patch-musashi-fsave-an.py"
    COMMENT "Copy pinned Musashi CPU with FSAVE-(An) boot fix"
    VERBATIM)
  add_library(qrx_musashi STATIC "${QRX_MUSASHI_PATCHED_DIR}/m68kcpu.c"
    "${QRX_GENERATED_DIR}/m68kops.c" "${QRX_MUSASHI_DIR}/softfloat/softfloat.c")
  target_include_directories(qrx_musashi PUBLIC "${QRX_MUSASHI_DIR}" "${QRX_GENERATED_DIR}")
  target_compile_definitions(qrx_musashi PRIVATE M68K_INSTRUCTION_HOOK=1 M68K_EMULATE_INT_ACK=1 M68K_EMULATE_ADDRESS_ERROR=1)
  # BSD setjmp saves the signal mask with two syscalls; m68k_execute takes one
  # per time slice.
  if(APPLE)
    target_compile_definitions(qrx_musashi PRIVATE setjmp=_setjmp longjmp=_longjmp)
  endif()
  target_link_libraries(qrx_musashi PUBLIC m)
  add_library(qrx68100_full STATIC full68k/qrx68100_full.c full68k/qrx68100_dual.c full68k/qrx68100_parallel.c full68k/qrx68100_vampire.c full68k/qrx68100_jit.c)
  target_include_directories(qrx68100_full PUBLIC full68k include "${QRX_MUSASHI_DIR}")
  # The JIT reads Musashi's CPU struct, so it shares that library's options.
  set_source_files_properties(full68k/qrx68100_jit.c PROPERTIES COMPILE_DEFINITIONS
    "M68K_INSTRUCTION_HOOK=1;M68K_EMULATE_INT_ACK=1;M68K_EMULATE_ADDRESS_ERROR=1")
  # Core 1 of the 68100: its own copy of Musashi and the JIT with every global renamed,
  # so it runs on a second host thread and core 0 pays nothing for it.
  set(QRX_CORE1_RENAME "${CMAKE_CURRENT_BINARY_DIR}/qrx_core1_rename.h")
  add_custom_command(OUTPUT "${QRX_CORE1_RENAME}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen-core1-rename.py"
      "$<TARGET_FILE:qrx_musashi>" "${QRX_CORE1_RENAME}"
    DEPENDS qrx_musashi "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen-core1-rename.py"
    COMMENT "Generate symbol renames for 68100 core 1" VERBATIM)
  add_custom_target(qrx_core1_rename DEPENDS "${QRX_CORE1_RENAME}")
  add_library(qrx_core1 STATIC full68k/qrx68100_core1.c full68k/qrx68100_jit.c
    "${QRX_MUSASHI_PATCHED_DIR}/m68kcpu.c" "${QRX_GENERATED_DIR}/m68kops.c" "${QRX_MUSASHI_DIR}/softfloat/softfloat.c")
  add_dependencies(qrx_core1 qrx_core1_rename)
  target_include_directories(qrx_core1 PRIVATE full68k include "${QRX_MUSASHI_DIR}" "${QRX_GENERATED_DIR}")
  target_compile_definitions(qrx_core1 PRIVATE M68K_INSTRUCTION_HOOK=1 M68K_EMULATE_INT_ACK=1 M68K_EMULATE_ADDRESS_ERROR=1)
  target_compile_options(qrx_core1 PRIVATE -include "${QRX_CORE1_RENAME}")
  if(APPLE)
    target_compile_definitions(qrx_core1 PRIVATE setjmp=_setjmp longjmp=_longjmp)
  endif()
  target_link_libraries(qrx68100_full PUBLIC qrx68100_core qrx_musashi qrx_core1)
  target_compile_options(qrx68100_full PRIVATE -Wall -Wextra -Wpedantic)
  # Full 68040 QRX68: same HUNK/Exec/DOS HLE as portable interpreter,
  # but the CPU, exceptions, instructions and FPU come from real Musashi.
  # Separated executable because Musashi owns process-global CPU callbacks.
  add_executable(qrx68-musashi-run qrx68/qrx68_main.c qrx68/qrx68_musashi.c)
  target_compile_definitions(qrx68-musashi-run PRIVATE QRX68_MUSASHI_ENABLED=1)
  target_include_directories(qrx68-musashi-run PRIVATE include "${QRX_MUSASHI_DIR}")
  target_link_libraries(qrx68-musashi-run PRIVATE qrx68_hle qrx_musashi)
  target_compile_options(qrx68-musashi-run PRIVATE -Wall -Wextra -Wpedantic)
  add_executable(qrx68-musashi-exceptions tests/qrx68_musashi_exception_tests.c qrx68/qrx68_musashi.c)
  target_include_directories(qrx68-musashi-exceptions PRIVATE include "${QRX_MUSASHI_DIR}")
  target_compile_definitions(qrx68-musashi-exceptions PRIVATE QRX68_MUSASHI_ENABLED=1)
  target_link_libraries(qrx68-musashi-exceptions PRIVATE qrx68_hle qrx_musashi)
  target_compile_options(qrx68-musashi-exceptions PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
  add_test(NAME QRX68_REAL_MUSASHI_68040_ILLEGAL_supervisor_RTE_exception
    COMMAND qrx68-musashi-exceptions)
  # The door builds the same x86 interpreter the guest runs, for the host.
  set(QRX86_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest/x86-runtime-22.5.12")
  file(GLOB QRX86_QMATH_SOURCES "${QRX86_RUNTIME_DIR}/qmath/*.c")
  set_source_files_properties(${QRX86_QMATH_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-std=gnu11")
  add_executable(qrx68100_full_run full68k/full_main.c full68k/qrx86_door.c full68k/qrx_hostnet.c full68k/qrx_hostapi.c full68k/qrx_hostapi_gl.c full68k/qrx_hostapi_glide.c full68k/qrx_hostapi_d3d.c full68k/qrx_hostapi_vk.c ${QRX86_QMATH_SOURCES})
  target_include_directories(qrx68100_full_run PRIVATE "${QRX86_RUNTIME_DIR}/core")
  set_source_files_properties(full68k/qrx86_door.c PROPERTIES COMPILE_OPTIONS "-w;-std=gnu11")
  target_link_libraries(qrx68100_full_run PRIVATE qrx68100_full)
  # Headers for the Vulkan wrapper: brew install vulkan-headers. The driver, MoltenVK, is only
  # looked for at run time.
  find_path(QRX_VULKAN_INCLUDE vulkan/vulkan.h HINTS /opt/homebrew/include /usr/local/include "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include")
  if(NOT QRX_VULKAN_INCLUDE)
    message(FATAL_ERROR "vulkan/vulkan.h not found: brew install vulkan-headers")
  endif()
  set_source_files_properties(full68k/qrx_hostapi_vk.c PROPERTIES INCLUDE_DIRECTORIES "${QRX_VULKAN_INCLUDE}")
  if(APPLE)
    target_sources(qrx68100_full_run PRIVATE macos/qrx_aros_window.m macos/qrx_metal_renderer.m macos/qrx_hostgpu.m macos/qrx_midi.m)
    set_source_files_properties(macos/qrx_aros_window.m macos/qrx_metal_renderer.m macos/qrx_hostgpu.m macos/qrx_midi.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_include_directories(qrx68100_full_run PRIVATE macos)
    target_link_libraries(qrx68100_full_run PRIVATE ${COCOA_FRAMEWORK} "-framework Metal" "-framework MetalKit" "-framework QuartzCore" "-framework CoreVideo" "-framework AudioToolbox")
  endif()
  add_executable(qrx68100_cpu_bench full68k/cpu_bench.c)
  target_link_libraries(qrx68100_cpu_bench PRIVATE qrx68100_full)
  add_executable(qrx68100_jit_diff_tests tests/jit_diff_tests.c)
  target_link_libraries(qrx68100_jit_diff_tests PRIVATE qrx68100_full)
  # Reads Musashi's CPU struct for the FPU state, so it shares that library's options.
  target_compile_definitions(qrx68100_jit_diff_tests PRIVATE M68K_INSTRUCTION_HOOK=1 M68K_EMULATE_INT_ACK=1 M68K_EMULATE_ADDRESS_ERROR=1)
  target_compile_options(qrx68100_jit_diff_tests PRIVATE -UNDEBUG -Wall -Wextra)
  add_test(NAME jit_a64_differential_against_musashi COMMAND qrx68100_jit_diff_tests)
  add_executable(qrx86_jit_tests tests/qrx86_jit_tests.c ${QRX86_QMATH_SOURCES})
  target_include_directories(qrx86_jit_tests PRIVATE "${QRX86_RUNTIME_DIR}/core" full68k include)
  target_link_libraries(qrx86_jit_tests PRIVATE qrx68100_full)
  target_compile_options(qrx86_jit_tests PRIVATE -w -std=gnu11 -UNDEBUG)
  add_test(NAME qrx86_host_jit_differential_against_interpreter COMMAND qrx86_jit_tests)
  add_executable(qrx68100_core1_tests tests/core1_tests.c)
  target_link_libraries(qrx68100_core1_tests PRIVATE qrx68100_full)
  target_compile_options(qrx68100_core1_tests PRIVATE -UNDEBUG -Wall -Wextra)
  add_test(NAME core1_second_host_thread_atomics_and_faults COMMAND qrx68100_core1_tests)
  add_executable(qrx_hostnet_tests tests/hostnet_tests.c full68k/qrx_hostnet.c)
  target_include_directories(qrx_hostnet_tests PRIVATE include)
  target_compile_options(qrx_hostnet_tests PRIVATE -UNDEBUG -Wall -Wextra)
  add_test(NAME winsock_host_sockets_tcp_udp_errors COMMAND qrx_hostnet_tests)
  add_executable(qrx68100_full_tests full68k/full_tests.c)
  target_link_libraries(qrx68100_full_tests PRIVATE qrx68100_full)
  target_compile_options(qrx68100_full_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
  add_test(NAME full_68k_cpu_boot_monitor_and_memory_bus COMMAND qrx68100_full_tests)
  add_executable(qrx68100_dual_demo full68k/dual_demo.c)
  target_link_libraries(qrx68100_dual_demo PRIVATE qrx68100_full)
  add_executable(qrx68100_parallel_bench full68k/parallel_bench.c)
  target_link_libraries(qrx68100_parallel_bench PRIVATE qrx68100_full)
  add_executable(qrx68100_dual_real_tests full68k/dual_real_tests.c)
  target_link_libraries(qrx68100_dual_real_tests PRIVATE qrx68100_full)
  target_compile_options(qrx68100_dual_real_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
  add_test(NAME dual_040_real_independent_cores_and_coherent_jit COMMAND qrx68100_dual_real_tests)
endif()

# Local hermetic regression of the new host bus; intentionally a mock CPU,
# clearly separated from the actual Musashi ISA tests above.
add_library(qrx_full_bus_mock STATIC full68k/qrx68100_full.c full68k/qrx68100_dual.c full68k/qrx68100_parallel.c full68k/qrx68100_vampire.c tests/musashi_stub/m68k_stub.c)
target_include_directories(qrx_full_bus_mock PUBLIC full68k include PRIVATE tests/musashi_stub)
target_link_libraries(qrx_full_bus_mock PUBLIC qrx68100_core)
add_executable(qrx_full_bus_mock_tests tests/full68k_bus_mock_tests.c)
target_link_libraries(qrx_full_bus_mock_tests PRIVATE qrx_full_bus_mock)
target_compile_options(qrx_full_bus_mock_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME qrx_full68k_host_bus_mock_NO_CPU_EXECUTION COMMAND qrx_full_bus_mock_tests)

add_executable(qrx68100_dual_mock_tests tests/dual_mock_tests.c)
target_include_directories(qrx68100_dual_mock_tests PRIVATE tests/musashi_stub)
target_link_libraries(qrx68100_dual_mock_tests PRIVATE qrx_full_bus_mock)
target_compile_options(qrx68100_dual_mock_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME dual_68040_compat_and_jit_coherence_MOCK COMMAND qrx68100_dual_mock_tests)

# ARMITHLON 1.6: two real host pthread workers; native A64 JIT when available,
# C fallback when not. This tests LOOP semantics with a mock Musashi context,
# not full 68040 parallel execution or a guest OS boot.
find_package(Threads REQUIRED)
target_link_libraries(qrx_full_bus_mock PUBLIC Threads::Threads)
if(TARGET qrx68100_full)
  target_link_libraries(qrx68100_full PUBLIC Threads::Threads)
endif()
add_executable(qrx68100_parallel_fast_tests tests/parallel_fast_tests.c)
target_include_directories(qrx68100_parallel_fast_tests PRIVATE tests/musashi_stub)
target_link_libraries(qrx68100_parallel_fast_tests PRIVATE qrx_full_bus_mock)
target_compile_options(qrx68100_parallel_fast_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME QRX68100_two_host_thread_native_fast_path_MOCK_68k COMMAND qrx68100_parallel_fast_tests)

# ARMITHLON 1.7: Vampire-inspired QRX-only 64-bit SIMD and functional JIT L1.
# This test uses a Musashi stub: it does not prove 68080 opcode or 040 FPU ISA.
add_executable(qrx68100_vampire_mock_tests tests/vampire_mock_tests.c)
target_include_directories(qrx68100_vampire_mock_tests PRIVATE tests/musashi_stub)
target_link_libraries(qrx68100_vampire_mock_tests PRIVATE qrx_full_bus_mock)
target_compile_options(qrx68100_vampire_mock_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME QRX68100_Vampire_inspired_cache_vector64_and_legacy_MOCK COMMAND qrx68100_vampire_mock_tests)

add_executable(qrx68100_vampire_microbench full68k/vampire_bench.c)
target_include_directories(qrx68100_vampire_microbench PRIVATE tests/musashi_stub)
target_link_libraries(qrx68100_vampire_microbench PRIVATE qrx_full_bus_mock)
target_compile_options(qrx68100_vampire_microbench PRIVATE -Wall -Wextra -Wpedantic)

# ARMITHLON 1.8: self-contained bootable 68k guest firmware and isolated QRXFS disk.
# This is a real CPU-executed 68k shell (limited instruction subset), not AROS.
add_executable(qrx68100_boot_shell bootshell/qrx_bootshell_main.c)
target_link_libraries(qrx68100_boot_shell PRIVATE qrx68100_core)
target_compile_options(qrx68100_boot_shell PRIVATE -Wall -Wextra -Wpedantic)
add_executable(qrx68100_boot_shell_tests tests/boot_shell_tests.c)
target_link_libraries(qrx68100_boot_shell_tests PRIVATE qrx68100_core)
target_compile_options(qrx68100_boot_shell_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME QRX68100_real_68k_boot_shell_and_persistent_QRXFS COMMAND qrx68100_boot_shell_tests)
if(QRX_ENABLE_MUSASHI)
  add_executable(qrx68100_boot_shell_full bootshell/qrx_bootshell_main.c)
  target_compile_definitions(qrx68100_boot_shell_full PRIVATE QRX_BOOT_USE_MUSASHI=1)
  target_link_libraries(qrx68100_boot_shell_full PRIVATE qrx68100_full)
  target_compile_options(qrx68100_boot_shell_full PRIVATE -Wall -Wextra -Wpedantic)
endif()

# v1.9: interpreter dispatch used by the native host integration and by the
# QRXDesktop launch profile. AROS runs its own QRX-C launcher; this host-side
# program is not an Exec/DOS replacement or a full native guest application.
add_library(qrx_app_router STATIC launcher/qrx_app.c)
target_include_directories(qrx_app_router PUBLIC launcher)
target_compile_options(qrx_app_router PRIVATE -Wall -Wextra -Wpedantic)
add_executable(qrx-app launcher/qrx_app_main.c)
target_link_libraries(qrx-app PRIVATE qrx_app_router)
add_executable(qrx_app_router_tests tests/app_router_tests.c)
target_link_libraries(qrx_app_router_tests PRIVATE qrx_app_router)
target_compile_options(qrx_app_router_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME qrx_pe32_elf32_hunk_qex_binary_signature_launcher COMMAND qrx_app_router_tests)
add_executable(qrx_block_device_tests tests/block_device_tests.c)
target_link_libraries(qrx_block_device_tests PRIVATE qrx68100_core)
target_compile_options(qrx_block_device_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME qrx_native_board_block_sector_dma_and_safe_persistence COMMAND qrx_block_device_tests)
if(QRX_ENABLE_MUSASHI AND Python3_Interpreter_FOUND)
  add_test(NAME REAL_MUSASHI_040_boot_QEX_and_two_core_worker
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-full68k-boot19.py"
    $<TARGET_FILE:qrx68100_boot_shell_full>)
endif()
if(Python3_Interpreter_FOUND)
  add_test(NAME qrx_existing_AROS_QRXDesktop_safe_doubleclick_source_patch
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrxdesktop-patch19.py")
endif()

# ARMITHLON 2.0: real shared-host-directory controller, not a disk image.
add_executable(qrx_hostfs_tests tests/hostfs_tests.c)
target_link_libraries(qrx_hostfs_tests PRIVATE qrx68100_core)
target_compile_options(qrx_hostfs_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME qrx_HOST_directory_volume_MMIO_DMA_and_confinement COMMAND qrx_hostfs_tests)
# Portable scheduling primitive for an AROS SMP adapter. Not integrated with
# Exec.library's own task switching, so the test is explicitly labeled.
add_library(qrx_smp_ready STATIC bsp/qrx_smp_ready.c)
target_include_directories(qrx_smp_ready PUBLIC include)
add_executable(qrx_smp_ready_tests tests/smp_ready_tests.c)
target_link_libraries(qrx_smp_ready_tests PRIVATE qrx_smp_ready Threads::Threads)
target_compile_options(qrx_smp_ready_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME QRX_SMP_shared_ready_queue_affinity_parallel_host_NO_AROS_EXEC COMMAND qrx_smp_ready_tests)

if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68100_boot_host_directory_no_IMG_READWRITE_and_guest_QEX
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-hostfs-boot20.py"
    $<TARGET_FILE:qrx68100_boot_shell>)
endif()
add_executable(qrx_hostfs_upper_alias_mock_tests tests/hostfs_fullalias_mock_tests.c)
target_include_directories(qrx_hostfs_upper_alias_mock_tests PRIVATE full68k include)
target_link_libraries(qrx_hostfs_upper_alias_mock_tests PRIVATE qrx_full_bus_mock)
target_compile_options(qrx_hostfs_upper_alias_mock_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME QRX68040_FF000800_hostfs_alias_and_DMA_CPU_MOCK COMMAND qrx_hostfs_upper_alias_mock_tests)

# 2.1: sparse host-backed RAM up to the 32-bit guest address ceiling;
# no fake physical memory beyond the 68040 address bus.
add_executable(qrx_memory_tests tests/memory_tests.c)
target_link_libraries(qrx_memory_tests PRIVATE qrx68100_core)
target_compile_options(qrx_memory_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME QRX68100_host_quarter_ram_sparse_128MiB_32bit_bus COMMAND qrx_memory_tests)
add_executable(qrx_hunk_loader_tests tests/hunk_loader_tests.c)
target_link_libraries(qrx_hunk_loader_tests PRIVATE qrx68100_core)
target_compile_options(qrx_hunk_loader_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME AmigaOS3_AROS68k_HUNK_CODE_DATA_RELOC32_strict_loader COMMAND qrx_hunk_loader_tests)

if(Python3_Interpreter_FOUND)
  add_test(NAME AmigaOS3_real_Kickstart_FS_UAE_compatibility_profile_configuration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-amigaos3-profile21.py")
endif()

if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68100_actual_guest_Amiga_HUNK_HOST_launch_96MiB
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-hunk-boot21.py"
      $<TARGET_FILE:qrx68100_boot_shell>)
endif()

add_executable(qrx_large_mem_040_mock_tests tests/large_mem_040_mock_tests.c)
target_link_libraries(qrx_large_mem_040_mock_tests PRIVATE qrx_full_bus_mock)
target_include_directories(qrx_large_mem_040_mock_tests PRIVATE full68k include tests/musashi_stub)
target_compile_options(qrx_large_mem_040_mock_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME QRX68040_128MiB_RAM_Kickstart_ROM_protection_AND_MOCK_CPU COMMAND qrx_large_mem_040_mock_tests)

if(Python3_Interpreter_FOUND AND NOT APPLE)
  add_test(NAME QRX_user_workstation_auto_RAM_HOST_readonly_no_IMG
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-workstation21.py")
endif()

# ARMITHLON 2.2: isolated AmigaOS 3.x HUNK + Exec/DOS HLE compatibility layer.
# The portable fallback executes a limited 68k ISA; the optional Musashi CPU
# integration remains separate. Do NOT present it as a complete OS environment.
add_library(qrx68_hle STATIC qrx68/qrx68_hle.c)
target_include_directories(qrx68_hle PUBLIC include)
target_link_libraries(qrx68_hle PUBLIC qrx68100_core)
target_compile_options(qrx68_hle PRIVATE -Wall -Wextra -Wpedantic)
add_executable(qrx68-run qrx68/qrx68_main.c)
target_link_libraries(qrx68-run PRIVATE qrx68_hle)
target_compile_options(qrx68-run PRIVATE -Wall -Wextra -Wpedantic)
if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68_QRXDesktop_2_4_patch_compatibility_and_source_guard
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrxdesktop-qrx68-24.py")
  add_test(NAME QRX68_AROS_guest_installer_non_destructive
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx68-aros-install24.py")
endif()
# QRX68/1 transport runs a REAL portable HUNK as an isolated host process.
# No AROS guest claims: separate AROS SDK/FS-UAE integration test is required.
if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68_guest_to_host_bridge_real_HUNK_and_GUI_snapshot
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx68-bridge24.py"
      $<TARGET_FILE:qrx68-run>)
endif()
add_executable(qrx68_hle_tests tests/qrx68_hle_tests.c)
target_link_libraries(qrx68_hle_tests PRIVATE qrx68_hle)
target_compile_options(qrx68_hle_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME QRX68_real_HUNK_Exec_DOS_library_vector_HLE_AND_shared_files COMMAND qrx68_hle_tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68_host_application_router_dispatches_real_HUNK_to_isolated_HLE
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx68-route.py"
      $<TARGET_FILE:qrx-app> $<TARGET_FILE:qrx68-run>)
endif()
if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68_amigaos_HUNK_batch_compatibility_failsafe_scanner
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx68-compat-scan.py"
      $<TARGET_FILE:qrx68-run>)
endif()

# ARMITHLON 2.3: real 68k HUNK -> opt-in Intuition/graphics drawing subset.
# The fast portable tests work offline; genuine 040 tests register ONLY after
# pinned third-party Musashi is present and QRX_ENABLE_MUSASHI=ON.
if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68_Amiga_HUNK_Intuition_graphics_HLE_guest_pixel_proof
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx68-gui23.py"
      $<TARGET_FILE:qrx68-run> "${CMAKE_CURRENT_SOURCE_DIR}/../data/qrx68/QRX68-GUI.hunk")
  if(QRX_ENABLE_MUSASHI)
    add_test(NAME QRX68_REAL_MUSASHI_68040_HUNK_Exec_DOS_Intuition_graphics
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx68-musashi23.py"
        $<TARGET_FILE:qrx68-musashi-run>
        "${CMAKE_CURRENT_SOURCE_DIR}/../data/qrx68/QRX68-Hello.hunk"
        "${CMAKE_CURRENT_SOURCE_DIR}/../data/qrx68/QRX68-GUI.hunk")
  endif()
endif()
# ABI compile/link regression with an explicit NONEXECUTING Musashi stub.
# This test is necessary for portable CI but is NOT a CPU or FPU execution test.
add_executable(qrx68_musashi_abi_linkcheck qrx68/qrx68_main.c
  qrx68/qrx68_musashi.c tests/musashi_qrx68_api_stub/m68k.c)
target_include_directories(qrx68_musashi_abi_linkcheck PRIVATE include
  tests/musashi_qrx68_api_stub)
target_compile_definitions(qrx68_musashi_abi_linkcheck PRIVATE QRX68_MUSASHI_ENABLED=1)
target_link_libraries(qrx68_musashi_abi_linkcheck PRIVATE qrx68_hle)
target_compile_options(qrx68_musashi_abi_linkcheck PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME QRX68_Musashi_68040_integration_COMPILE_LINK_ONLY_STUB
  COMMAND qrx68_musashi_abi_linkcheck --help)
if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68_AMIGA_68k_Intuition_graphics_to_native_Q86F_GPU_packet
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx68-metal23.py"
      $<TARGET_FILE:qrx68-run> "${CMAKE_CURRENT_SOURCE_DIR}/../data/qrx68/QRX68-GUI.hunk")
endif()

if(Python3_Interpreter_FOUND)
  add_test(NAME QRXDesktop_6_6_20_existing_UI_HUNK_compat_ToolType_optin_patch
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrxdesktop-qrx68-23.py")
endif()
add_executable(qrx68_hunk_large_tests tests/qrx68_hunk_large_tests.c)
target_link_libraries(qrx68_hunk_large_tests PRIVATE qrx68_hle)
target_compile_options(qrx68_hunk_large_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME QRX68_large_Amiga_HUNK_96KiB_heap_guard_no_program_overlap COMMAND qrx68_hunk_large_tests)

# ARMITHLON 2.5: real portable HUNK IDCMP event loop + live QRX68/2 transport.
if(Python3_Interpreter_FOUND)
  add_test(NAME QRX68_2_live_IDCMP_mouse_redraw_close_concurrent_HUNK
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-qrx68-live25.py"
      $<TARGET_FILE:qrx68-run>)
endif()

# 2.6: Real library-vector ABI tests for signals, ports, gadgets, window z-order.
add_executable(qrx68_compat26_tests tests/qrx68_workbench_compat26.c)
target_link_libraries(qrx68_compat26_tests PRIVATE qrx68_hle)
target_compile_options(qrx68_compat26_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME QRX68_26_Exec_signals_ports_Intuition_gadget_menu_layered_windows
         COMMAND qrx68_compat26_tests)
