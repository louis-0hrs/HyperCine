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
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET CFHDCodecStatic)
    add_executable(test_small_chroma_oracle test_small_chroma_oracle.cpp)
    target_link_libraries(test_small_chroma_oracle PRIVATE CFHDCodecStatic)
    add_test(NAME small_chroma_upstream_oracle COMMAND test_small_chroma_oracle)
    # Instrument a separate compile of the pristine translation unit. This
    # keeps production SDK flags and upstream files unchanged.
    option(HYPERCINE_SMALL_CHROMA_ASAN "Build the manual upstream prefix ASan diagnostic" OFF)
    if(HYPERCINE_SMALL_CHROMA_ASAN)
        add_executable(test_small_chroma_oracle_asan test_small_chroma_oracle.cpp
            ${CMAKE_SOURCE_DIR}/upstream/Codec/spatial.c)
        target_compile_definitions(test_small_chroma_oracle_asan PRIVATE
            HYPERCINE_ORACLE_ASAN=1 _ALLOCATOR=1 WARPSTUFF=1
            _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
        target_compile_options(test_small_chroma_oracle_asan PRIVATE /fsanitize=address /Zi)
        target_link_options(test_small_chroma_oracle_asan PRIVATE /INCREMENTAL:NO /DEBUG)
        target_link_libraries(test_small_chroma_oracle_asan PRIVATE CFHDCodecStatic)
    endif()
endif()

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
    add_test(NAME test_diagnostic_geometry_guard COMMAND test_phase2a --self-test-geometry-guard)

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

if(HYPERCINE_ENABLE_CUDA AND WIN32)
    add_executable(hypercine_reference_export ../tools/reference_corpus_export.cpp)
    target_link_libraries(hypercine_reference_export PRIVATE hypercine_wavelet CFHDCodecStatic bcrypt)
    target_compile_definitions(hypercine_reference_export PRIVATE
        HYPERCINE_SOURCE_COMMIT="${HYPERCINE_SOURCE_COMMIT}"
        HYPERCINE_SOURCE_DIRTY=${HYPERCINE_SOURCE_DIRTY}
        HYPERCINE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
endif()

# Actual GoPro decode of packets produced by the independent SDK-free encoder.
# Windows CI uses the existing reference workflow; no workflow changes needed.
if(WIN32 AND TARGET CFHDCodecStatic)
    add_executable(test_standalone_sdk_oracle test_standalone_sdk_oracle.cpp)
    target_link_libraries(test_standalone_sdk_oracle PRIVATE CFHDCodecStatic)
    set(oracle_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/standalone")
    add_test(NAME standalone_oracle_fixture_hashes COMMAND "${CMAKE_COMMAND}"
        "-DFIXTURES=${oracle_fixtures}" -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_oracle_fixtures.cmake")
    add_test(NAME standalone_sdk_decode_3_packets COMMAND test_standalone_sdk_oracle
        "${oracle_fixtures}" "${CMAKE_CURRENT_BINARY_DIR}/oracle-ctest")
    set_tests_properties(standalone_sdk_decode_3_packets PROPERTIES
        DEPENDS standalone_oracle_fixture_hashes TIMEOUT 120
        PASS_REGULAR_EXPRESSION "SDK decode PASS: all three standalone packets"
        FAIL_REGULAR_EXPRESSION "SDK decode FAIL;control FAIL;Oracle harness error")
    # Existing CI prints only failed CTest output and uploads no artifacts.
    # Emit an additional diagnostic report (including binary evidence) into its
    # build log. The CTest above independently repeats and gates real decoding.
    option(HYPERCINE_ORACLE_BUILD_REPORT "Print SDK oracle diagnostic/binary evidence during build" ON)
    if(HYPERCINE_ORACLE_BUILD_REPORT)
        add_custom_target(standalone_oracle_evidence ALL
            COMMAND "${CMAKE_COMMAND}" "-DFIXTURES=${oracle_fixtures}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_oracle_fixtures.cmake"
            COMMAND $<TARGET_FILE:test_standalone_sdk_oracle> "${oracle_fixtures}"
                "${CMAKE_CURRENT_BINARY_DIR}/oracle-build" --report
            DEPENDS test_standalone_sdk_oracle VERBATIM)
    endif()
endif()
