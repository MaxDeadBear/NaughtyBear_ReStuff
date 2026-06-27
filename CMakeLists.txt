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
    add_executable(restuff WIN32 ${RESTUFF_SOURCES})
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
