cmake_minimum_required(VERSION 3.21)
project(wmr VERSION 1.4.0 LANGUAGES CXX)

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
    src/video/scene_detector.cpp
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

# Homebrew's fftw3 ships a variable-only CMake config (FFTW3f_LIBRARIES is the
# bare library name) with no imported target, whereas vcpkg provides FFTW3::fftw3f.
# Synthesize the target from the variables when it is missing so the test
# executable (which links FFTW3::fftw3f) builds under both providers. No-op when
# the provider already supplies the target (e.g. vcpkg).
if(FFTW3f_FOUND AND NOT TARGET FFTW3::fftw3f)
    add_library(FFTW3::fftw3f INTERFACE IMPORTED)
    set_target_properties(FFTW3::fftw3f PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${FFTW3f_INCLUDE_DIRS}"
        INTERFACE_LINK_DIRECTORIES "${FFTW3f_LIBRARY_DIRS}"
        INTERFACE_LINK_LIBRARIES "${FFTW3f_LIBRARIES}")
endif()

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
    FFMPEG::swresample
)
target_include_directories(${PROJECT_NAME} PRIVATE ${FFTW3f_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE ${FFTW3f_LIBRARY_DIRS})

# macOS system frameworks required by FFmpeg
if(APPLE)
    find_library(COREMEDIA_LIB CoreMedia)
    find_library(COREVIDEO_LIB CoreVideo)
    find_library(COREAUDIO_LIB CoreAudio)
    find_library(VIDEOTOOLBOX_LIB VideoToolbox)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox)
    find_library(ACCELERATE_LIB Accelerate)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${COREMEDIA_LIB}
        ${COREVIDEO_LIB}
        ${COREAUDIO_LIB}
        ${VIDEOTOOLBOX_LIB}
        ${COREFOUNDATION_LIB}
        ${AUDIOTOOLBOX_LIB}
        ${ACCELERATE_LIB}
    )
endif()

# Windows system libraries required by FFmpeg
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ws2_32 crypt32 bcrypt secur32 ncrypt mfplat mfuuid strmiids
    )
endif()

# x264 is a transitive dependency of FFmpeg's libx264 codec (static linking)
find_library(X264_LIBRARY NAMES x264 PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib NO_DEFAULT_PATH)
if(NOT X264_LIBRARY)
    find_library(X264_LIBRARY NAMES x264)
endif()
if(X264_LIBRARY)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${X264_LIBRARY})
endif()

# Platform-specific flags
if(APPLE)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
elseif(UNIX)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(${PROJECT_NAME} PRIVATE -static-libgcc -static-libstdc++)
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(VCPKG_TARGET_TRIPLET MATCHES "static")
        set_property(TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
endif()

# Release optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(NOT MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE -O3)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE /O2)
    endif()
endif()

# AI denoise (FDnCNN via NCNN/Vulkan) — opt-in. OFF by default keeps the lean
# default build volk/NCNN-free. Runs BEFORE the tests subdirectory so the volk
# imported target exists when tests/CMakeLists.txt's mirror block executes.
option(WMR_BUILD_AI_DENOISE "Build FDnCNN AI denoise (NCNN/Vulkan)" OFF)
if(WMR_BUILD_AI_DENOISE)
    # The NCNN submodule must be present.
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/ncnn/ncnn-20260113-src/CMakeLists.txt")
        message(FATAL_ERROR "NCNN submodule missing. Run: git submodule update --init --recursive")
    endif()

    # volk provides the Vulkan loader bindings used by the ncnn_shim.
    find_package(volk CONFIG REQUIRED)

    # Diagnostic-only: surfaces where CMake's FindVulkan resolved the loader.
    find_package(Vulkan QUIET)

    # Configure the in-tree NCNN as a static lib with Vulkan on. Cache knobs
    # are set BEFORE add_subdirectory so NCNN picks them up.
    set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
    set(NCNN_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(NCNN_OPENMP OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NCNN_ENABLE_LTO OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/external/ncnn/ncnn-20260113-src ncnn)

    target_sources(${PROJECT_NAME} PRIVATE
        src/core/ai_denoise.cpp
        src/core/ai_denoise_model.cpp
    )

    if(TARGET volk::volk)
        set(WMR_VOLK_TARGET volk::volk)
    else()
        set(WMR_VOLK_TARGET volk)
    endif()

    target_link_libraries(${PROJECT_NAME} PRIVATE ncnn ${WMR_VOLK_TARGET})
    target_compile_definitions(${PROJECT_NAME} PRIVATE WMR_AI_DENOISE=1)
endif()

# Tests
option(WMR_BUILD_TESTS "Build test suite" ON)
if(WMR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
