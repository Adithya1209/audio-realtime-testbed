cmake_minimum_required(VERSION 3.20)
project(AudioRealtimeTestbed CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(RTAUDIO_API_WASAPI ON CACHE BOOL "Build WASAPI API" FORCE)
    set(RTAUDIO_API_DS OFF CACHE BOOL "Build DirectSound API" FORCE)
elseif(APPLE)
    set(RTAUDIO_API_CORE ON CACHE BOOL "Build CoreAudio API" FORCE)
else()
    # Linux (ALSA / PulseAudio)
    set(RTAUDIO_API_ALSA ON CACHE BOOL "Build ALSA API" FORCE)
    set(RTAUDIO_API_PULSE ON CACHE BOOL "Build PulseAudio API" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG 6.0.1
)
FetchContent_MakeAvailable(rtaudio)

add_executable(audio_testbed main.cpp)
target_link_libraries(audio_testbed PRIVATE rtaudio)