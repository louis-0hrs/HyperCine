# HyperCine Reference Test Suite CMakeLists.txt

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/upstream/Common
    ${CMAKE_SOURCE_DIR}/upstream/Codec
    ${CMAKE_SOURCE_DIR}/upstream/EncoderSDK
    ${CMAKE_SOURCE_DIR}/upstream/DecoderSDK
    ${CMAKE_SOURCE_DIR}/upstream/Example
    ${CMAKE_SOURCE_DIR}/upstream/Example/WaveletDemo
)

# 1. Transform-level test
add_executable(test_transform_wavelet
    test_transform_wavelet.cpp
    ${CMAKE_SOURCE_DIR}/upstream/Example/WaveletDemo/utils.c
)
add_test(NAME test_transform_wavelet COMMAND test_transform_wavelet)

# 2. Codec-level roundtrip test
add_executable(test_codec_roundtrip
    test_codec_roundtrip.cpp
)
target_link_libraries(test_codec_roundtrip PRIVATE CFHDCodecStatic)
add_test(NAME test_codec_roundtrip COMMAND test_codec_roundtrip)

add_executable(test_yuy2_stability
    test_yuy2_stability.cpp
)
target_link_libraries(test_yuy2_stability PRIVATE CFHDCodecStatic)
add_test(NAME test_yuy2_stability COMMAND test_yuy2_stability)

# 3. Upstream sample AVI decode test
add_executable(test_avi_sample
    test_avi_sample.cpp
    ${CMAKE_SOURCE_DIR}/upstream/Example/readavi.cpp
    ${CMAKE_SOURCE_DIR}/upstream/Example/mp4reader.cpp
    ${CMAKE_SOURCE_DIR}/upstream/Example/fileio.cpp
)
target_link_libraries(test_avi_sample PRIVATE CFHDCodecStatic)
add_test(NAME test_avi_sample
    COMMAND test_avi_sample "${CMAKE_SOURCE_DIR}/upstream/Example/videos/CFHDsample422.AVI"
)

# 4. Phase 1A CUDA wavelet transform test
if(HYPERCINE_ENABLE_CUDA)
    add_executable(test_cuda_wavelet
        test_cuda_wavelet.cpp
    )
    target_link_libraries(test_cuda_wavelet PRIVATE hypercine_core)
    add_test(NAME test_cuda_wavelet COMMAND test_cuda_wavelet)
endif()


if(HYPERCINE_ENABLE_CUDA)
    add_executable(test_cuda_pyramid test_cuda_pyramid.cpp)
    target_link_libraries(test_cuda_pyramid PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_cuda_pyramid COMMAND test_cuda_pyramid)

    add_executable(test_entropy_parallel test_entropy_parallel.cpp)
    target_link_libraries(test_entropy_parallel PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_entropy_parallel COMMAND test_entropy_parallel)

    add_executable(test_hybrid_roundtrip test_hybrid_roundtrip.cpp)
    target_link_libraries(test_hybrid_roundtrip PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_hybrid_roundtrip COMMAND test_hybrid_roundtrip)

    add_executable(test_phase2a test_phase2a.cpp)
    target_link_libraries(test_phase2a PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_phase2a COMMAND test_phase2a)

    add_executable(test_phase2b test_phase2b.cpp)
    target_link_libraries(test_phase2b PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_phase2b COMMAND test_phase2b)

    add_executable(test_phase2c test_phase2c.cpp)
    target_link_libraries(test_phase2c PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_phase2c COMMAND test_phase2c)

    add_executable(test_phase2d test_phase2d.cpp)
    target_link_libraries(test_phase2d PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_phase2d COMMAND test_phase2d)

    add_executable(test_entropy_capacity test_entropy_capacity.cpp)
    target_link_libraries(test_entropy_capacity PRIVATE hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_entropy_capacity PRIVATE psapi)
    endif()
    add_test(NAME test_entropy_capacity COMMAND test_entropy_capacity)

    add_executable(test_phase3a test_phase3a.cpp)
    target_link_libraries(test_phase3a PRIVATE hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_phase3a PRIVATE psapi)
    endif()
    add_test(NAME test_phase3a COMMAND test_phase3a)
endif()

# Decode actual native Mac-produced 1080p FS3 fixtures with pristine GoPro SDK.
# This is independent of the sibling PR #9 and does not merge its fixtures/code.
if(WIN32 AND TARGET CFHDCodecStatic)
    add_test(NAME filmscan3_fixture_integrity COMMAND "${CMAKE_COMMAND}"
        "-DROOT=${CMAKE_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_filmscan3_fixtures.cmake")
    add_executable(test_filmscan3_windows_decoder test_filmscan3_windows_decoder.cpp)
    target_link_libraries(test_filmscan3_windows_decoder PRIVATE CFHDCodecStatic)
    add_test(NAME test_filmscan3_windows_decoder COMMAND test_filmscan3_windows_decoder
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/macos-filmscan3/flat.cfhd"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/macos-filmscan3/gradient.cfhd")
    set_tests_properties(test_filmscan3_windows_decoder PROPERTIES DEPENDS filmscan3_fixture_integrity)
endif()
