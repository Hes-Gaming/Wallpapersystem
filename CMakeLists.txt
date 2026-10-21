cmake_minimum_required(VERSION 3.20)
project(WallpaperSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(WallpaperSystem WIN32
    src/main.cpp
    src/config.cpp
    src/video_decoder.cpp
    src/renderer.cpp
    src/pause_watcher.cpp
)

target_compile_definitions(WallpaperSystem PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(WallpaperSystem PRIVATE /utf-8 /W4 /permissive-)
    # Only the software fallback compiles shaders; keep the DLL off the hardware path.
    target_link_options(WallpaperSystem PRIVATE /DELAYLOAD:d3dcompiler_47.dll)
    target_link_libraries(WallpaperSystem PRIVATE delayimp)
    set_property(TARGET WallpaperSystem PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(WallpaperSystem PRIVATE
    d3d11
    dxgi
    d3dcompiler
    mfplat
    mfreadwrite
    mfuuid
    dwmapi
    ole32
    user32
    gdi32
)

# Keep the executable next to config.md and the wallpapers folder.
set_target_properties(WallpaperSystem PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}"
)
