cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
project(JarkViewOpus LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Vendored JarkViewer core sources (jark006/JarkViewer, GPL-3.0)
# ---------------------------------------------------------------------------
set(JARK_SRC
    jark/src/ImageDatabase.cpp
    jark/src/FormatSniffer.cpp
    jark/src/jarkUtils.cpp
    jark/src/exifParse.cpp
    jark/src/ColorManager.cpp
    jark/src/Localization.cpp
    jark/src/stringRes.cpp
    jark/src/MediaDecoder.cpp
    jark/src/videoDecoder.cpp
    jark/src/MediaPlayer.cpp
    jark/src/AudioOutput.cpp
    jark/src/AudioSpectrumAnalyzer.cpp
    jark/src/PsdDecoder.cpp
    jark/src/VectorImage.cpp
    jark/src/blpDecoder.cpp
    jark/src/AiPrompt.cpp
    jark/src/tinyxml2.cpp
)

add_library(JarkViewOpus SHARED
    src/JarkViewOpus.cpp
    ${JARK_SRC}
)

target_include_directories(JarkViewOpus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/include
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/include/libwebp2
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/include/ffmpeg
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk
)

target_compile_definitions(JarkViewOpus PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# /utf-8: vendored sources contain Chinese comments.
# /std:c++23preview: upstream JarkViewer requires C++23 (MSVC v145).
# /MT: the vendored static libs are built with static CRT; mixing CRTs breaks linking.
target_compile_options(JarkViewOpus PRIVATE
    /std:c++23preview
    /utf-8
    /EHsc
    /Zc:__cplusplus
    /W1
)

set_property(TARGET JarkViewOpus PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Vendored prebuilt static libs live in these directories (fetched in CI from
# jark006/JarkViewer release tag static_lib). #pragma comment(lib) inside the
# vendored headers selects the libraries; these dirs make them findable.
target_link_directories(JarkViewOpus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/libavif
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/libexiv2
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/libffmpeg
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/libjxl
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/libopencv
    ${CMAKE_CURRENT_SOURCE_DIR}/jark/libwebp2
)

target_link_libraries(JarkViewOpus PRIVATE
    ole32
    oleaut32
    uuid
    gdi32
    user32
    advapi32
    winmm
    dwmapi
    shlwapi
)

set_target_properties(JarkViewOpus PROPERTIES
    OUTPUT_NAME "JarkViewOpus"
)
