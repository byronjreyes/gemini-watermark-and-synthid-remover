cmake_minimum_required(VERSION 3.21)

find_package(Catch2 REQUIRED)

# Test sources
set(TEST_SOURCES
    unit/blend_modes_test.cpp
    unit/fft_context_test.cpp
    unit/spectral_codebook_test.cpp
    unit/inpaint_test.cpp
    integration/visible_pipeline_test.cpp
)

# Library sources needed by tests
set(LIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/blend_modes.cpp
    ${CMAKE_SOURCE_DIR}/src/core/watermark_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/core/inpaint.cpp
    ${CMAKE_SOURCE_DIR}/src/core/fft_context.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/ncc_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/synthid_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/synthid/spectral_codebook.cpp
    ${CMAKE_SOURCE_DIR}/src/synthid/codebook_subtractor.cpp
    ${CMAKE_SOURCE_DIR}/src/synthid/codebook_builder.cpp
    # NOTE: src/video/scene_detector.cpp is intentionally excluded — it pulls in
    # VideoReader (FFmpeg), which the test target does not link. No test
    # references SceneDetector today; re-add it (with the FFMPEG link) if a
    # scene-detection test is added.
)

add_executable(wmr_tests ${TEST_SOURCES} ${LIB_SOURCES})

target_include_directories(wmr_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/assets
)

target_link_libraries(wmr_tests PRIVATE
    Catch2::Catch2WithMain
    opencv_core
    opencv_imgcodecs
    opencv_imgproc
    opencv_photo
    fmt::fmt
    spdlog::spdlog
    FFTW3::fftw3f
)

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(wmr_tests)
