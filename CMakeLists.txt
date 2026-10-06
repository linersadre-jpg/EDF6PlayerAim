cmake_minimum_required(VERSION 3.20)
project(EDF6PlayerAim VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# edf6common: the memory / patch / image code the plugin links (common/)
add_subdirectory(common)

# The plugin itself. It uses its own sources plus edf6common and the loader's API, and nothing else.
add_subdirectory(plugin)
