# restuff - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(restuff LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(RESTUFF_SOURCES
    src/main.cpp
    src/hooks.cpp
    src/native_vk.cpp                    # M1.1 native Vulkan graphics system + M1.2b draw layer
    src/renderer/native_backend_vk.cpp   # M1.2a guest life-support: MMIO claim, shadow regs, mini-PM4, interrupt pump
    src/renderer/guest_d3d_hooks.cpp     # M1.2b guest-D3D UP-draw capture + decode (strong-symbol overrides)
    src/renderer/guest_texture_decode.cpp # M1.2b Xenos texture untile + CPU decode to RGBA8
    src/renderer/ucode_translator.cpp    # M2.3 Xenos microcode -> GLSL translator
    src/renderer/shader_pipeline.cpp     # M2.3 runtime GLSL -> SPIR-V (libshaderc) + shader cache
)
if(WIN32)
    # C23 roundeven* for the recompiler's PPC round-to-even ops; the msvc-wine
    # UCRT (10.0.26100) does not export them. glibc has them, so Linux skips it.
    list(APPEND RESTUFF_SOURCES src/rex_crt_shim.cpp)
    # The Windows rexruntime.dll ships with NO Vulkan code at all (D3D12-only
    # runtime -- zero VulkanProvider/vulkan-1.dll mentions in the DLL), while
    # this project *is* the SDK's Vulkan provider + our native renderer. So the
    # provider layer is compiled straight into the exe from the SDK source tree
    # at the EXACT commit the installed SDK was built from (8dadea6; a git
    # worktree at ~/Git/rexglue-sdk-8dadea6 pinned there).
    # M4.0: overridable so a NATIVE Windows build can point at a local copy of
    # the same worktree; the default keeps the Linux cross build unchanged.
    set(REXSDK_SOURCE_TREE "/home/Tynan/Git/rexglue-sdk-8dadea6" CACHE PATH
        "rexglue-sdk source tree pinned at the installed SDK's commit (8dadea6)")
    set(REXSDK_SRC "${REXSDK_SOURCE_TREE}/src")
    file(GLOB REXSDK_VULKAN_SOURCES "${REXSDK_SRC}/ui/vulkan/*.cpp")
    # spirv_tools_context needs the spirv-tools LIBRARY, which the win SDK
    # package does not ship, and nothing we link requires it -- exclude.
    list(REMOVE_ITEM REXSDK_VULKAN_SOURCES
         "${REXSDK_SRC}/ui/vulkan/spirv_tools_context.cpp")
    list(APPEND RESTUFF_SOURCES ${REXSDK_VULKAN_SOURCES})
endif()

if(WIN32)
    # Compile DASHICON.ico into the executable so Explorer/taskbar/window use it.
    # The .rc is generated with an absolute icon path so it resolves regardless of
    # the build/working directory.
    enable_language(RC)
    set(RESTUFF_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/DASHICON.ico")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/restuff.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/restuff.rc @ONLY)
    add_executable(restuff WIN32 ${RESTUFF_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/restuff.rc)
else()
    add_executable(restuff ${RESTUFF_SOURCES})
endif()

# GPU_PLUGINS xenos stages the runtime-loaded Xenos GPU emulation plugin
# (rex::gpu-xenos) next to the executable. The new SDK extracted Xenos GPU
# emulation into this plugin, which ReXApp loads at startup when the
# `gpu_plugin` cvar is "xenos" (see RestuffApp::OnPreSetup and the staged
# restuff.toml). This project renders through the SDK's emulated GPU rather than
# its own renderer, so the plugin is required.
rexglue_setup_target(restuff GPU_PLUGINS xenos)

# M4.16 (configure with -DRESTUFF_RECOMP_FMA=1): fused-FMA recomp variant.
# Pairs with the REX_EMIT_FMA codegen gate -- fused simde_mm_fmadd_ps needs
# FMA3 at compile time or simde silently falls back to the unfused pair.
# -ffp-contract=off keeps clang from ALSO contracting unrelated mul+add
# chains, so ONLY the explicit intrinsics change rounding (clean A/B).
# Turn back OFF (-DRESTUFF_RECOMP_FMA=0) when building unfused codegen; the
# cache variable persists across configures. (Must sit after
# rexglue_setup_target -- that call creates the restuff_recomp target.)
option(RESTUFF_RECOMP_FMA "Compile recompiled TUs with FMA3 for fused vmaddfp emission" OFF)
# restuff_recomp only exists once codegen has produced generated/default/
# sources.cmake, so guard on the target: a FRESH CLONE configures before
# codegen has ever run, and an unguarded call hard-fails there.
if(RESTUFF_RECOMP_FMA AND TARGET restuff_recomp)
    target_compile_options(restuff_recomp PRIVATE -mfma -ffp-contract=off)
endif()

# native_vk.cpp pulls in the rex Vulkan provider/instance headers, and
# rex/ui/renderdoc_api.h does `#include <renderdoc_app.h>`. The SDK ships that
# third-party header under include/renderdoc/ (not on the default rex include
# path, so the stock hooks.cpp never needed it). Add it for our target.
get_filename_component(_rex_sdk_root "${rexglue_DIR}/../../.." ABSOLUTE)
target_include_directories(restuff PRIVATE "${_rex_sdk_root}/include/renderdoc")

# M2.3: libshaderc compiles translated guest GLSL -> SPIR-V at runtime (it
# statically bundles glslang, so no external toolchain is needed on end-user
# machines). Linux: system libshaderc_shared. For Windows distribution, ship
# shaderc_shared.dll next to the exe (packaging task).
# M4.0: HINTS the LunarG Vulkan SDK so a native Windows build finds its
# shaderc_shared.lib; no-op where the env var is unset (Linux cross build).
find_library(SHADERC_SHARED_LIB NAMES shaderc_shared REQUIRED
             HINTS "$ENV{VULKAN_SDK}/Lib")
target_link_libraries(restuff PRIVATE ${SHADERC_SHARED_LIB})

# Attract-mode video player (src/video_player.h): Media Foundation decodes the
# MP4 and XAudio2 plays its audio track. Windows-only feature, so the Linux
# build links nothing extra and video_player.h compiles its no-op path.
if(WIN32)
    target_link_libraries(restuff PRIVATE mfplat mfreadwrite mfuuid xaudio2)
endif()
if(WIN32)
    # Cross build: shaderc headers ship beside the prebuilt .lib (Linux gets
    # them from /usr/include implicitly), and the SDK's rex/ui/vulkan/api.h
    # needs <vulkan/vulkan.h> -- the Vulkan headers are platform-neutral, so a
    # shadow dir symlinking the system copies serves the MSVC-target compile.
    # M4.0: overridable for native Windows builds; falls back to the LunarG
    # Vulkan SDK's include dir when the cross-build shadow dir is absent.
    set(RESTUFF_VULKAN_INCLUDE
        "/run/media/Tynan/Data/Restuff Project/Rexglue SDK/vulkan-headers/include"
        CACHE PATH "Vulkan headers for the MSVC-target compile")
    if(NOT EXISTS "${RESTUFF_VULKAN_INCLUDE}" AND DEFINED ENV{VULKAN_SDK})
        set(RESTUFF_VULKAN_INCLUDE "$ENV{VULKAN_SDK}/Include")
    endif()
    get_filename_component(_shaderc_root "${SHADERC_SHARED_LIB}" DIRECTORY)
    target_include_directories(restuff PRIVATE
        "${_shaderc_root}/../include"
        "${RESTUFF_VULKAN_INCLUDE}"
        # header-only VulkanMemoryAllocator for the SDK's vulkan_mem_alloc.cpp
        "${REXSDK_SOURCE_TREE}/thirdparty/vulkan-memory-allocator/include")
    # The UCRT deprecation blanket (getenv/fopen/etc.) is noise for this
    # codebase; the Linux build is the primary and uses them deliberately.
    target_compile_definitions(restuff PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# GLSL -> SPIR-V -> embedded alignas(4) uint32_t headers for the native draw
# layer (util::CreateShaderModule takes const uint32_t*, so xxd-style char
# arrays are not alignment-safe; see cmake/embed_spv.cmake).
find_program(GLSLC glslc REQUIRED HINTS "$ENV{VULKAN_SDK}/Bin")
set(RESTUFF_SHADER_GEN ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${RESTUFF_SHADER_GEN})
set(RESTUFF_SHADER_HEADERS "")
function(restuff_embed_shader SHADER_FILE VAR)
    set(_src ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${SHADER_FILE})
    set(_spv ${RESTUFF_SHADER_GEN}/${SHADER_FILE}.spv)
    set(_hdr ${RESTUFF_SHADER_GEN}/${SHADER_FILE}.spv.h)
    add_custom_command(OUTPUT ${_hdr}
        COMMAND ${GLSLC} -O -o ${_spv} ${_src}
        COMMAND ${CMAKE_COMMAND} -DSPV=${_spv} -DOUT=${_hdr} -DVAR=${VAR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spv.cmake
        DEPENDS ${_src} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spv.cmake
        COMMENT "glslc ${SHADER_FILE} -> ${VAR}"
        VERBATIM)
    set(RESTUFF_SHADER_HEADERS ${RESTUFF_SHADER_HEADERS} ${_hdr} PARENT_SCOPE)
endfunction()
restuff_embed_shader(draw2d.vert kDraw2DVS)
restuff_embed_shader(draw2d_quad.frag kDraw2DQuadFS)
restuff_embed_shader(draw2d_text.frag kDraw2DTextFS)
add_custom_target(restuff_shaders DEPENDS ${RESTUFF_SHADER_HEADERS})
add_dependencies(restuff restuff_shaders)
target_include_directories(restuff PRIVATE ${RESTUFF_SHADER_GEN})

# Stage a default runtime config (windowed mode + gpu_plugin=xenos) next to the
# executable. Copied only when absent so user edits survive rebuilds; delete the
# staged copy to regenerate it from this template.
add_custom_command(TARGET restuff POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/restuff.toml
        -DDST=$<TARGET_FILE_DIR:restuff>/restuff.toml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_default_config.cmake
    COMMENT "Staging default restuff.toml (if absent)"
    VERBATIM
)
