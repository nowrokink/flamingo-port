cmake_minimum_required(VERSION 3.24)
project(flamingoport LANGUAGES C CXX)

# FlamingoPort.exe: the launcher (app/, ui/), the preparation (prep/, shaders/, xlat/, cache/, on core/) and the
# runtime (src/, shims/, gpu/) in one Windows-subsystem program; then the runtime's tests.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "FlamingoPort must be built for x64 (run from an x64 Native Tools prompt or vcvars64.bat)")
endif()

# libopus (BSD, Xiph) for the audio decoder's Opus packets: downloaded on the first configure, pinned by hash.
include(FetchContent)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
  URL https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
  URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(opus)

set(RT_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set(RT_OPTS /W4 /permissive- /utf-8)

# The runtime: what runs the game (FlamingoPort.exe --run).
set(RT_SOURCES
  src/main.cpp
  src/pe_loader.cpp
  src/run.cpp
  src/game_patches.cpp
  src/cpu_emul.cpp
  shims/registry.cpp
  shims/xmem.cpp
  shims/d3d12_xs.cpp
  shims/com_probe.cpp
  shims/xcrdapi.cpp
  shims/debug.cpp
  shims/advapi.cpp
  shims/loader.cpp
  shims/xgameruntime.cpp
  shims/xtaskqueue.cpp
  shims/files.cpp
  shims/net.cpp
  shims/storage.cpp
  shims/d3d12x.cpp
  shims/frame.cpp
  shims/uma.cpp
  shims/vmem.cpp
  gpu/gfx10_layout.cpp
  gpu/gfx10_swizzle.cpp
  gpu/sharp.cpp
  gpu/desc_table.cpp
  shims/d3d12x_views.cpp
  shims/d3d12x_bindless.cpp
  shims/d3d12x_objects.cpp
  shims/shader_cache.cpp
  shims/gpu_memory.cpp
  shims/d3d12x_pipelines.cpp
  shims/d3d12x_draw.cpp
  shims/d3d12x_diag.cpp
  shims/d3d12x_upscale.cpp
  shims/d3d12x_rects.cpp
  shims/d3d12x_upload.cpp
  gpu/stage_meta.cpp
  shims/xg.cpp
  shims/inflate.cpp
  shims/services.cpp
  shims/audio_hw.cpp
  shims/gamesave.cpp
  shims/window.cpp
  shims/pad.cpp
  shims/gameinput.cpp
  shims/audio_out.cpp
  shims/ffx.cpp
  shims/waits.cpp
  shims/bcpack.cpp
  shims/overlay.cpp
)
# The rest of the program: every .cpp of the launcher's and the preparation's folders.
set(FP_DIRS app core ui prep shaders xlat cache)
set(fp_sources "")
foreach(dir ${FP_DIRS})
  file(GLOB_RECURSE found CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp")
  list(APPEND fp_sources ${found})
endforeach()

# app/flamingo.rc: the icon and the launcher's art (assets/launcher), drawn for this project
add_executable(FlamingoPort WIN32 ${RT_SOURCES} ${fp_sources} app/flamingo.rc)
set_property(SOURCE app/flamingo.rc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../assets/launcher/flamingo.ico" "${CMAKE_CURRENT_SOURCE_DIR}/../assets/launcher/cover.png"
  "${CMAKE_CURRENT_SOURCE_DIR}/../assets/launcher/splash.png")
# Includes are written from this folder ("core/fs.h", "shims/inflate.h"); gen/ holds the generated header below.
target_include_directories(FlamingoPort PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/gen")
# The translator's identity (cache/builder.cpp): regenerated whenever a translator or cache source changes.
file(GLOB fp_xlat_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/xlat/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/cache/compile.*" "${CMAKE_CURRENT_SOURCE_DIR}/cache/builder.*")
set(fp_tools_id "${CMAKE_CURRENT_BINARY_DIR}/gen/fp_tools_id.h")
add_custom_command(OUTPUT "${fp_tools_id}"
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${fp_tools_id}" "-DFILES=${fp_xlat_sources}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tools_id.cmake"
  DEPENDS ${fp_xlat_sources} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tools_id.cmake"
  COMMENT "Hashing the translator's sources")
target_sources(FlamingoPort PRIVATE "${fp_tools_id}")
target_compile_definitions(FlamingoPort PRIVATE ${RT_DEFS})
# /Zc:threadSafeInit-: function-local statics must not use TLS, since the game exe takes over this exe's TLS block
# (index 0; see src/run.cpp).
target_compile_options(FlamingoPort PRIVATE ${RT_OPTS} /Zc:threadSafeInit-)
# Static shim tables register themselves at startup; keep the linker from dropping their objects.
target_link_options(FlamingoPort PRIVATE /OPT:NOREF)
# The launcher's own DLLs load when the launcher first calls them, never in a --run process: there the runtime needs
# the address space free (uma.cpp reserves its range from below 4 GiB, and what loads at startup can sit in the way).
set(launcher_dlls d2d1 dwrite windowscodecs shell32 bcrypt iphlpapi dwmapi)
foreach(dll ${launcher_dlls})
  target_link_options(FlamingoPort PRIVATE /DELAYLOAD:${dll}.dll)
endforeach()
# pdh (the overlay's GPU and disk counters) the same way: loaded by the overlay's thread, long after uma's range.
target_link_options(FlamingoPort PRIVATE /DELAYLOAD:pdh.dll)
target_link_libraries(FlamingoPort PRIVATE psapi pdh wer ws2_32 d3d12 dxgi dbghelp hid setupapi ole32 opus
  ${launcher_dlls} delayimp)

# Static-TLS slots lent to manually mapped modules (src/tls_slot.cpp): one DLL per module with TLS, loaded from
# beside the exe.
foreach(n RANGE 0 3)
  add_library(rt_tls_slot${n} SHARED src/tls_slot.cpp)
  target_compile_definitions(rt_tls_slot${n} PRIVATE ${RT_DEFS})
  target_compile_options(rt_tls_slot${n} PRIVATE ${RT_OPTS} /GS- /Zl)
  target_link_options(rt_tls_slot${n} PRIVATE /NODEFAULTLIB /ENTRY:DllMain /INCLUDE:_tls_used)
  target_link_libraries(rt_tls_slot${n} PRIVATE kernel32)
  set_target_properties(rt_tls_slot${n} PROPERTIES MSVC_RUNTIME_CHECKS "")
  add_dependencies(FlamingoPort rt_tls_slot${n})
endforeach()

# DEFLATE / zlib for DirectStorage requests.
add_executable(pad_test test/pad_test.cpp shims/pad.cpp)
target_compile_definitions(pad_test PRIVATE ${RT_DEFS})
target_compile_options(pad_test PRIVATE ${RT_OPTS})
target_link_libraries(pad_test PRIVATE hid setupapi)

add_executable(cpu_emul_test test/cpu_emul_test.cpp src/cpu_emul.cpp)
target_compile_definitions(cpu_emul_test PRIVATE ${RT_DEFS})
target_compile_options(cpu_emul_test PRIVATE ${RT_OPTS})

add_executable(inflate_test test/inflate_test.cpp shims/inflate.cpp)
target_compile_definitions(inflate_test PRIVATE ${RT_DEFS})
target_compile_options(inflate_test PRIVATE ${RT_OPTS})

# BCPack (DirectStorage's packed BC blocks); with a folder of samples, against rpf9-explorer's decoder.
add_executable(bcpack_test test/bcpack_test.cpp shims/bcpack.cpp)
target_compile_definitions(bcpack_test PRIVATE ${RT_DEFS})
target_compile_options(bcpack_test PRIVATE ${RT_OPTS})

# The console's texture layout (GetResourceAllocationInfo sizes) against retail texture sizes.
add_executable(gfx10_layout_test test/gfx10_layout_test.cpp gpu/gfx10_layout.cpp gpu/gfx10_swizzle.cpp gpu/sharp.cpp)
target_compile_definitions(gfx10_layout_test PRIVATE ${RT_DEFS})
target_compile_options(gfx10_layout_test PRIVATE ${RT_OPTS})

# xmem.dll shims on their own (no Vulkan needed).
add_executable(xmem_test test/xmem_test.cpp shims/registry.cpp shims/xmem.cpp shims/uma.cpp shims/vmem.cpp)
target_compile_definitions(xmem_test PRIVATE ${RT_DEFS})
target_compile_options(xmem_test PRIVATE ${RT_OPTS})
target_link_options(xmem_test PRIVATE /OPT:NOREF)
target_link_libraries(xmem_test PRIVATE psapi)

# Virtual drives (title paths on G:, T: ... -> host folders).
add_executable(files_test test/files_test.cpp shims/registry.cpp shims/files.cpp)
target_compile_definitions(files_test PRIVATE ${RT_DEFS})
target_compile_options(files_test PRIVATE ${RT_OPTS})
target_link_options(files_test PRIVATE /OPT:NOREF)
target_link_libraries(files_test PRIVATE wer)

# Offline by design: the title's network entry points fail.
add_executable(net_test test/net_test.cpp shims/registry.cpp shims/net.cpp)
target_compile_definitions(net_test PRIVATE ${RT_DEFS})
target_compile_options(net_test PRIVATE ${RT_OPTS})
target_link_options(net_test PRIVATE /OPT:NOREF)
target_link_libraries(net_test PRIVATE ws2_32)

# Persistent local storage, served through QueryApiImpl.
add_executable(storage_test test/storage_test.cpp shims/registry.cpp shims/com_probe.cpp shims/xgameruntime.cpp
  shims/xtaskqueue.cpp shims/files.cpp shims/storage.cpp)
target_compile_definitions(storage_test PRIVATE ${RT_DEFS})
target_compile_options(storage_test PRIVATE ${RT_OPTS})
target_link_options(storage_test PRIVATE /OPT:NOREF)
target_link_libraries(storage_test PRIVATE wer)

# XGameSave through its runtime area: write, read, partial update, enumerate, delete.
add_executable(gamesave_test test/gamesave_test.cpp shims/registry.cpp shims/com_probe.cpp shims/xgameruntime.cpp
  shims/xtaskqueue.cpp shims/gamesave.cpp)
target_compile_definitions(gamesave_test PRIVATE ${RT_DEFS})
target_compile_options(gamesave_test PRIVATE ${RT_OPTS})
target_link_options(gamesave_test PRIVATE /OPT:NOREF)

# The console frame clock (device slots 78-81), in real time.
add_executable(frame_test test/frame_test.cpp shims/frame.cpp)
target_compile_definitions(frame_test PRIVATE ${RT_DEFS})
target_compile_options(frame_test PRIVATE ${RT_OPTS})

# Prototype for docs/gpu-memory.md: pool chunks of title memory as D3D12 heaps (needs a D3D12 GPU).
add_executable(stage_meta_test test/stage_meta_test.cpp gpu/stage_meta.cpp)

add_executable(gpu_memory_test test/gpu_memory_test.cpp shims/gpu_memory.cpp shims/uma.cpp)
target_compile_definitions(gpu_memory_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(gpu_memory_test PRIVATE /utf-8)
target_link_libraries(gpu_memory_test PRIVATE d3d12 dxgi)

add_executable(uma_test test/uma_test.cpp)
target_compile_definitions(uma_test PRIVATE ${RT_DEFS})
target_compile_options(uma_test PRIVATE ${RT_OPTS})
target_link_libraries(uma_test PRIVATE d3d12 dxgi)

# The task-queue provider of xgameruntime (XTaskQueue, XAsync).
add_executable(xtaskqueue_test test/xtaskqueue_test.cpp shims/registry.cpp shims/com_probe.cpp
  shims/xgameruntime.cpp shims/xtaskqueue.cpp)
target_compile_definitions(xtaskqueue_test PRIVATE ${RT_DEFS})
target_compile_options(xtaskqueue_test PRIVATE ${RT_OPTS})
target_link_options(xtaskqueue_test PRIVATE /OPT:NOREF)

# ---- Vulkan layer (runtime/gpu). Only the headers are needed: vulkan-1.dll is loaded at runtime.
set(RT_VULKAN_INCLUDE "" CACHE PATH "Directory holding vulkan/vulkan.h (default: $VULKAN_SDK/Include)")
if(NOT RT_VULKAN_INCLUDE AND DEFINED ENV{VULKAN_SDK})
  set(RT_VULKAN_INCLUDE "$ENV{VULKAN_SDK}/Include")
endif()
if(NOT EXISTS "${RT_VULKAN_INCLUDE}/vulkan/vulkan.h")
  message(WARNING "Vulkan headers not found (install the Vulkan SDK or set RT_VULKAN_INCLUDE): skipping runtime/gpu")
else()
  add_library(rt_gpu STATIC
    gpu/vk.cpp
    gpu/device.cpp
    gpu/heap.cpp
    gpu/desc_table.cpp
    gpu/ssharp.cpp
    gpu/bindless.cpp
    gpu/vertex_input.cpp
    gpu/literal_samplers.cpp
  )
  target_include_directories(rt_gpu PUBLIC "${RT_VULKAN_INCLUDE}")
  target_compile_definitions(rt_gpu PUBLIC ${RT_DEFS})
  target_compile_options(rt_gpu PRIVATE ${RT_OPTS})

  add_executable(gpu_unit_test test/gpu_unit_test.cpp)
  target_link_libraries(gpu_unit_test PRIVATE rt_gpu)
  target_compile_options(gpu_unit_test PRIVATE ${RT_OPTS})

  add_executable(gpu_layer_test test/gpu_layer_test.cpp)
  target_link_libraries(gpu_layer_test PRIVATE rt_gpu)
  target_compile_options(gpu_layer_test PRIVATE ${RT_OPTS})
endif()
