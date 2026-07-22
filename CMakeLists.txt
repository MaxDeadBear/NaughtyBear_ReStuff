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
)

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

# Media Foundation (MP4/H.264 decode) + XAudio2 (the video's audio track) for
# the fullscreen video overlay (src/video_player.h). Both ship with Windows, so
# no thirdparty dependency.
if(WIN32)
    target_link_libraries(restuff PRIVATE mfplat mfreadwrite mfuuid xaudio2)
endif()

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
