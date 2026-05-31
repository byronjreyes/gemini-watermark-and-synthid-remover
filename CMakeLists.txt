cmake_minimum_required(VERSION 3.21)
project(wmr VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Sources
set(SOURCES
    src/main.cpp
    src/core/blend_modes.cpp
    src/core/watermark_engine.cpp
    src/core/inpaint.cpp
    src/core/fft_context.cpp
    src/detection/ncc_detector.cpp
    src/detection/synthid_detector.cpp
    src/synthid/spectral_codebook.cpp
    src/synthid/codebook_subtractor.cpp
    src/synthid/noise_residual_subtractor.cpp
    src/synthid/codebook_builder.cpp
    src/cli/cli_app.cpp
    src/cli/batch_processor.cpp
    src/video/video_reader.cpp
    src/video/video_writer.cpp
    src/video/video_processor.cpp
)

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/assets
)

# Compile definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_NAME="${PROJECT_NAME}"
)

# Find dependencies via vcpkg
find_package(OpenCV REQUIRED)
find_package(fmt REQUIRED)
find_package(CLI11 REQUIRED)
find_package(spdlog REQUIRED)
find_package(FFTW3f REQUIRED)
find_package(FFMPEG REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
    opencv_core
    opencv_imgcodecs
    opencv_imgproc
    opencv_photo
    fmt::fmt
    CLI11::CLI11
    spdlog::spdlog
    ${FFTW3f_LIBRARIES}
    FFMPEG::avformat
    FFMPEG::avcodec
    FFMPEG::avutil
    FFMPEG::swscale
)
target_include_directories(${PROJECT_NAME} PRIVATE ${FFTW3f_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE ${FFTW3f_LIBRARY_DIRS})

# Platform-specific flags
if(APPLE)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
elseif(UNIX)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(${PROJECT_NAME} PRIVATE -static-libgcc -static-libstdc++)
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_property(TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Release optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(NOT MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE -O3)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE /O2)
    endif()
endif()

# Tests
option(WMR_BUILD_TESTS "Build test suite" ON)
if(WMR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
