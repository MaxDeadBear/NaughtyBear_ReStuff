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
list(APPEND RESTUFF_SOURCES icon/restuff.rc)
    add_executable(restuff WIN32 ${RESTUFF_SOURCES})
else()
    add_executable(restuff ${RESTUFF_SOURCES})
endif()

rexglue_setup_target(restuff)

if(WIN32)
    target_link_libraries(restuff PRIVATE
        mfplat
        mfreadwrite
        mfuuid
        xaudio2
    )
endif()