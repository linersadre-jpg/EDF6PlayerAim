cmake_minimum_required(VERSION 3.20)
project(EDF6PlayerAim VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# A standalone plugin: it uses only src/ of its own directory plus edf6common (common/) and the loader's API.
# It does not link, include or write anything of EDF6VehicleCrew or EDF6AutoTurret.
add_library(EDF6PlayerAim SHARED src/plugin.cpp)
target_link_libraries(EDF6PlayerAim PRIVATE edf6common)
target_include_directories(EDF6PlayerAim PRIVATE ${CMAKE_SOURCE_DIR}/third_party/EDFModLoader)
target_compile_options(EDF6PlayerAim PRIVATE /W4 /WX /utf-8 /permissive-)
target_link_options(EDF6PlayerAim PRIVATE /DYNAMICBASE /NXCOMPAT /PDBALTPATH:EDF6PlayerAim.pdb)
set_target_properties(EDF6PlayerAim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Mods/Plugins")
configure_file(EDF6PlayerAim.ini "${CMAKE_BINARY_DIR}/Mods/Plugins/EDF6PlayerAim.ini" COPYONLY)
