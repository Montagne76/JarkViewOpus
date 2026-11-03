cmake_minimum_required(VERSION 3.20)
project(JarkViewOpus LANGUAGES CXX)

add_library(JarkViewOpus SHARED src/JarkViewOpus.cpp)

target_include_directories(JarkViewOpus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk)

target_compile_definitions(JarkViewOpus PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _CRT_SECURE_NO_WARNINGS)

# /EHsc: exceptions must not escape into the host (Directory Opus) process.
target_compile_options(JarkViewOpus PRIVATE /EHsc /W3)

target_link_libraries(JarkViewOpus PRIVATE
    ole32          # CoInitializeEx / CoCreateInstance
    windowscodecs  # WIC factory GUIDs
    uuid
    gdi32          # DIB section
    user32)

set_target_properties(JarkViewOpus PROPERTIES
    OUTPUT_NAME "JarkViewOpus"
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
