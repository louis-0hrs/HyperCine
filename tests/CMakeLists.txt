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

if(NOT HYPERCINE_ENABLE_CUDA)
    add_executable(test_decoder_cpu test_decoder_cpu.cpp)
    target_link_libraries(test_decoder_cpu PRIVATE HyperCine::Decoder)
    add_test(NAME test_decoder_cpu COMMAND test_decoder_cpu)
endif()

add_executable(test_decode_phase3b2a_parser test_decode_phase3b2a_parser.cpp)
target_include_directories(test_decode_phase3b2a_parser PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(test_decode_phase3b2a_parser PRIVATE HyperCine::Decoder)
add_test(NAME test_decode_phase3b2a_parser COMMAND test_decode_phase3b2a_parser "${CMAKE_SOURCE_DIR}")

if(HYPERCINE_ENABLE_CUDA)
    add_executable(test_decode_phase3b2a_api test_decode_phase3b2a_api.cpp)
    target_link_libraries(test_decode_phase3b2a_api PRIVATE HyperCine::Decoder CFHDCodecStatic)
    add_test(NAME test_decode_phase3b2a_api COMMAND test_decode_phase3b2a_api "${CMAKE_SOURCE_DIR}")

    add_executable(test_decode_phase3b2a_reconstruction test_decode_phase3b2a_reconstruction.cpp)
    target_link_libraries(test_decode_phase3b2a_reconstruction PRIVATE HyperCine::Decoder CFHDCodecStatic)
    add_test(NAME test_decode_phase3b2a_reconstruction COMMAND test_decode_phase3b2a_reconstruction "${CMAKE_SOURCE_DIR}")

    add_executable(test_decode_phase3b2a_temporal test_decode_phase3b2a_temporal.cu)
    target_include_directories(test_decode_phase3b2a_temporal PRIVATE ${CMAKE_SOURCE_DIR}/src/cuda)
    target_link_libraries(test_decode_phase3b2a_temporal PRIVATE hypercine_wavelet)
    add_test(NAME test_decode_phase3b2a_temporal COMMAND test_decode_phase3b2a_temporal)

    add_executable(test_decode_phase3b2a_upstream_temporal test_decode_phase3b2a_upstream_temporal.cpp)
    target_link_libraries(test_decode_phase3b2a_upstream_temporal PRIVATE CFHDCodecStatic)
    add_test(NAME test_decode_phase3b2a_upstream_temporal COMMAND test_decode_phase3b2a_upstream_temporal)

    add_executable(test_decode_phase3ae test_decode_phase3ae.cpp)
    target_compile_definitions(test_decode_phase3ae PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase3ae PRIVATE HyperCine::Decoder hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_decode_phase3ae PRIVATE psapi)
    endif()
    add_test(NAME test_decode_phase3ae COMMAND test_decode_phase3ae --unit
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/phase3ae-unit")
    set_tests_properties(test_decode_phase3ae PROPERTIES TIMEOUT 120)

    add_executable(test_decode_phase3a_rc test_decode_phase3a_rc.cpp
        ${CMAKE_SOURCE_DIR}/upstream/Example/readavi.cpp
        ${CMAKE_SOURCE_DIR}/upstream/Example/mp4reader.cpp
        ${CMAKE_SOURCE_DIR}/upstream/Example/fileio.cpp)
    target_compile_definitions(test_decode_phase3a_rc PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase3a_rc PRIVATE HyperCine::Decoder hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_decode_phase3a_rc PRIVATE psapi)
    endif()
    add_test(NAME test_decode_phase3a_rc COMMAND test_decode_phase3a_rc --validate
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rc-results")
    set_tests_properties(test_decode_phase3a_rc PROPERTIES TIMEOUT 600)
endif()

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

add_executable(test_decode_phase0 test_decode_phase0.cpp)
target_compile_definitions(test_decode_phase0 PRIVATE _ALLOCATOR=1)
target_link_libraries(test_decode_phase0 PRIVATE CFHDCodecStatic)
add_test(NAME test_decode_phase0 COMMAND test_decode_phase0
    "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
set_tests_properties(test_decode_phase0 PROPERTIES TIMEOUT 300)

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
    add_executable(test_decoder test_decoder.cpp)
    target_compile_definitions(test_decoder PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decoder PRIVATE HyperCine::Decoder hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_decoder PRIVATE psapi)
    endif()
    add_test(NAME test_decoder COMMAND test_decoder "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decoder PROPERTIES TIMEOUT 600)

    add_executable(test_decode_phase3b0
        test_decode_phase3b0.cpp
        seh_guard.c
        ${CMAKE_SOURCE_DIR}/upstream/Example/readavi.cpp
        ${CMAKE_SOURCE_DIR}/upstream/Example/mp4reader.cpp
        ${CMAKE_SOURCE_DIR}/upstream/Example/fileio.cpp)
    target_compile_definitions(test_decode_phase3b0 PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase3b0 PRIVATE HyperCine::Decoder hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_decode_phase3b0 PRIVATE psapi)
    endif()
    add_test(NAME test_decode_phase3b0 COMMAND test_decode_phase3b0
        "${CMAKE_CURRENT_BINARY_DIR}/phase3b0-regression/results"
        "${CMAKE_CURRENT_BINARY_DIR}/phase3b0-regression/fixtures"
        "${CMAKE_SOURCE_DIR}")
    set_tests_properties(test_decode_phase3b0 PROPERTIES TIMEOUT 1800)

    add_executable(test_decode_phase3b1 test_decode_phase3b1.cpp)
    target_compile_definitions(test_decode_phase3b1 PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase3b1 PRIVATE HyperCine::Decoder hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_decode_phase3b1 PRIVATE psapi)
    endif()
    add_test(NAME test_decode_phase3b1 COMMAND test_decode_phase3b1
        "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/benchmarks/results")
    set_tests_properties(test_decode_phase3b1 PROPERTIES TIMEOUT 1800)

    add_executable(test_decode_phase3b1_acceptance test_decode_phase3b1_acceptance.cpp)
    target_compile_definitions(test_decode_phase3b1_acceptance PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase3b1_acceptance PRIVATE HyperCine::Decoder hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_decode_phase3b1_acceptance PRIVATE psapi)
    endif()
    add_test(NAME test_decode_phase3b1_acceptance COMMAND test_decode_phase3b1_acceptance
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/phase3b1-acceptance" --matrix)
    add_test(NAME test_decode_phase3b2a_mixed_stress COMMAND test_decode_phase3b1_acceptance
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/phase3b2a-mixed-stress" --stress)
    set_tests_properties(test_decode_phase3b1_acceptance test_decode_phase3b2a_mixed_stress PROPERTIES TIMEOUT 1800)

    add_executable(test_decode_phase3b2r test_decode_phase3b2r.cpp)
    target_compile_definitions(test_decode_phase3b2r PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase3b2r PRIVATE HyperCine::Decoder hypercine_wavelet CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(test_decode_phase3b2r PRIVATE psapi)
    endif()
    add_test(NAME test_decode_phase3b2r_math COMMAND test_decode_phase3b2r --math)
    add_test(NAME test_decode_phase3b2r_reference COMMAND test_decode_phase3b2r --reference-smoke
        "${CMAKE_SOURCE_DIR}")
    set_tests_properties(test_decode_phase3b2r_reference PROPERTIES TIMEOUT 180)

    add_executable(test_decode_phase2fa test_decode_phase2fa.cpp)
    target_compile_definitions(test_decode_phase2fa PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2fa PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase2fa COMMAND test_decode_phase2fa
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase2fa PROPERTIES TIMEOUT 1800)
    add_executable(test_decode_gpu_index test_decode_gpu_index.cu)
    target_link_libraries(test_decode_gpu_index PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_gpu_index COMMAND test_decode_gpu_index)
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

    add_executable(test_decode_inverse_wavelet_cuda test_decode_inverse_wavelet_cuda.cpp)
    target_compile_definitions(test_decode_inverse_wavelet_cuda PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_inverse_wavelet_cuda PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_inverse_wavelet_cuda COMMAND test_decode_inverse_wavelet_cuda)

    add_executable(test_decode_phase1 test_decode_phase1.cpp)
    target_compile_definitions(test_decode_phase1 PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase1 PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase1 COMMAND test_decode_phase1
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase1 PROPERTIES TIMEOUT 300)

    add_executable(test_color_conversion_cuda test_color_conversion_cuda.cu)
    target_link_libraries(test_color_conversion_cuda PRIVATE hypercine_wavelet)
    add_test(NAME test_color_conversion_cuda COMMAND test_color_conversion_cuda)

    add_executable(test_decode_phase2a test_decode_phase2a.cpp)
    target_compile_definitions(test_decode_phase2a PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2a PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase2a COMMAND test_decode_phase2a
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase2a PROPERTIES TIMEOUT 300)

    add_executable(test_decode_entropy_cuda test_decode_entropy_cuda.cu)
    target_link_libraries(test_decode_entropy_cuda PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_entropy_cuda COMMAND test_decode_entropy_cuda)

    add_executable(test_decode_phase2b test_decode_phase2b.cpp)
    target_compile_definitions(test_decode_phase2b PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2b PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase2b COMMAND test_decode_phase2b
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase2b PROPERTIES TIMEOUT 300)

    add_executable(prototype_phase2c_research prototype_phase2c_research.cu)
    target_compile_definitions(prototype_phase2c_research PRIVATE _ALLOCATOR=1)
    target_link_libraries(prototype_phase2c_research PRIVATE hypercine_wavelet CFHDCodecStatic)

    add_executable(test_decode_entropy_phase2c test_decode_entropy_phase2c.cu)
    target_link_libraries(test_decode_entropy_phase2c PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_entropy_phase2c COMMAND test_decode_entropy_phase2c)

    add_executable(test_decode_phase2c test_decode_phase2c.cpp)
    target_compile_definitions(test_decode_phase2c PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2c PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase2c COMMAND test_decode_phase2c
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase2c PROPERTIES TIMEOUT 300)

    add_executable(test_decode_phase2d test_decode_phase2d.cpp)
    target_compile_definitions(test_decode_phase2d PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2d PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase2d COMMAND test_decode_phase2d
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase2d PROPERTIES TIMEOUT 300)

    add_executable(test_decode_phase2e test_decode_phase2e.cpp)
    target_compile_definitions(test_decode_phase2e PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2e PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase2e COMMAND test_decode_phase2e
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase2e PROPERTIES TIMEOUT 300)

    add_executable(test_decode_phase2fc test_decode_phase2fc.cpp)
    target_compile_definitions(test_decode_phase2fc PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2fc PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase2fc COMMAND test_decode_phase2fc
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase2fc PROPERTIES TIMEOUT 400)

    add_executable(test_decode_phase2fc_audit test_decode_phase2fc_audit.cpp)
    target_compile_definitions(test_decode_phase2fc_audit PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2fc_audit PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_decode_phase2fc_audit COMMAND test_decode_phase2fc_audit
        "${CMAKE_CURRENT_SOURCE_DIR}/decode_phase0_golden.txt")
    set_tests_properties(test_decode_phase2fc_audit PROPERTIES TIMEOUT 400)
endif()



if(HYPERCINE_ENABLE_CUDA AND HYPERCINE_BUILD_BENCHMARKS)
    add_executable(test_decode_phase2fb test_decode_phase2fb.cpp)
    target_compile_definitions(test_decode_phase2fb PRIVATE _ALLOCATOR=1)
    target_link_libraries(test_decode_phase2fb PRIVATE decode_phase2fb_research)
    add_test(NAME test_decode_phase2fb COMMAND test_decode_phase2fb)
    set_tests_properties(test_decode_phase2fb PROPERTIES TIMEOUT 300)
endif()

# Diagnostics-only helper for Compute Sanitizer evidence (issue #13). It is a
# kernel-instrumentation control, not a codec test, so it registers no CTest:
# run it manually under compute-sanitizer. See docs/DECODE_PHASE3B2A.md.
if(HYPERCINE_ENABLE_CUDA)
    add_executable(sanitizer_launch_control sanitizer_launch_control.cu)
    set_target_properties(sanitizer_launch_control PROPERTIES CUDA_ARCHITECTURES ${CMAKE_CUDA_ARCHITECTURES})
endif()
