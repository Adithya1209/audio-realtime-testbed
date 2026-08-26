cmake_minimum_required(VERSION 3.20)
project(AudioRealtimeTestbed CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force WASAPI backend on Windows
set(RTAUDIO_API_WASAPI ON CACHE BOOL "Build WASAPI API" FORCE)

include(FetchContent)
FetchContent_Declare(
    rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG 6.0.1
)
FetchContent_MakeAvailable(rtaudio)

add_executable(audio_testbed main.cpp)
target_link_libraries(audio_testbed PRIVATE rtaudio)