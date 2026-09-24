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

