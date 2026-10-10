cmake_minimum_required(VERSION 3.28)
project(SnapLiteToolchainSmoke LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "This validation target requires Windows and MSVC.")
endif()

add_executable(snaplite-toolchain-smoke main.cpp)
target_compile_features(snaplite-toolchain-smoke PRIVATE cxx_std_20)
target_compile_definitions(snaplite-toolchain-smoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(snaplite-toolchain-smoke PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus)
set_property(TARGET snaplite-toolchain-smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(snaplite-toolchain-smoke PRIVATE user32 ole32 d2d1 dwrite windowscodecs windowsapp)
