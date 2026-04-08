
# restuff - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
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

rexglue_setup_target(restuff)
