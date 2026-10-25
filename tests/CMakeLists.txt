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

# 0. SDK-free fail-closed reference-oracle contract.
# Runs on every platform and does not require the original GoPro SDK.
add_executable(test_reference_oracle test_reference_oracle.cpp)
add_test(NAME test_reference_oracle COMMAND test_reference_oracle)
set_tests_properties(test_reference_oracle PROPERTIES LABELS "reference-oracle")

# Optional validation tooling only; the codec has no Python dependency.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_reference_release_inventory
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_reference_release.py")
    set_tests_properties(test_reference_release_inventory PROPERTIES LABELS "reference-oracle")
    # The runner must use the CTest that configured this build. CMAKE_CTEST_COMMAND
    # is only set by include(CTest); this project calls enable_testing() directly.
    if(CMAKE_CTEST_COMMAND)
        set(HYPERCINE_CTEST_COMMAND "${CMAKE_CTEST_COMMAND}")
    else()
        find_program(HYPERCINE_CTEST_COMMAND NAMES ctest HINTS "${CMAKE_ROOT}/bin")
    endif()
    if(NOT HYPERCINE_CTEST_COMMAND)
        message(FATAL_ERROR "ctest executable not found for reference_release_gate")
    endif()
    # Explicit qualification entrypoint: unlike ordinary CTest, a skipped or
    # missing Phase 2 reference test makes this target fail (runner exit 77).
    add_custom_target(reference_release_gate
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_reference_release.py"
            --build "${CMAKE_BINARY_DIR}" --config "$<CONFIG>"
            --ctest "${HYPERCINE_CTEST_COMMAND}"
            --output "${CMAKE_BINARY_DIR}/reference-release.json"
        DEPENDS test_reference_oracle
        USES_TERMINAL VERBATIM)

    # Recorded RCA-159 band-0 witness (#234). Recomputed statistics and the
    # sanitized records only; no private corpus and no GPU are involved.
    add_test(NAME test_rca159_witness_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_rca159_witness.py")
    set_tests_properties(test_rca159_witness_contract PROPERTIES LABELS "validation")
endif()

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
    # Exit 77 means the strict encoder gates passed but the original-SDK pixel
    # gate is BLOCKED_ORACLE. CTest reports it as skipped, never as passed.
    set_tests_properties(test_phase2a PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "reference-oracle"
    )

    add_executable(test_phase2b test_phase2b.cpp)
    target_link_libraries(test_phase2b PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_phase2b COMMAND test_phase2b)
    set_tests_properties(test_phase2b PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "reference-oracle"
    )

    add_executable(test_phase2c test_phase2c.cpp)
    target_link_libraries(test_phase2c PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_phase2c COMMAND test_phase2c)
    set_tests_properties(test_phase2c PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "reference-oracle"
    )

    add_executable(test_phase2d test_phase2d.cpp)
    target_link_libraries(test_phase2d PRIVATE hypercine_wavelet CFHDCodecStatic)
    add_test(NAME test_phase2d COMMAND test_phase2d)
    set_tests_properties(test_phase2d PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "reference-oracle"
    )
    if(TARGET reference_release_gate)
        add_dependencies(reference_release_gate test_phase2a test_phase2b test_phase2c test_phase2d)
    endif()

    add_executable(test_entropy_capacity test_entropy_capacity.cpp)
    target_link_libraries(test_entropy_capacity PRIVATE hypercine_wavelet CFHDCodecStatic Threads::Threads)
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

# Issue #189: isolated original-SDK reference collector; no CUDA linkage.
if(WIN32 AND MSVC AND TARGET CFHDCodecStatic)
    add_executable(gop2_reference_export ../tools/gop2_reference_export.cpp)
    target_include_directories(gop2_reference_export PRIVATE "${CMAKE_SOURCE_DIR}/upstream/DecoderSDK")
    target_compile_definitions(gop2_reference_export PRIVATE _ALLOCATOR=1 WARPSTUFF=1
        HYPERCINE_SOURCE_COMMIT="${HYPERCINE_SOURCE_COMMIT}")
    target_compile_options(gop2_reference_export PRIVATE /EHs)
    target_link_libraries(gop2_reference_export PRIVATE CFHDCodecStatic)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME gop2_reference_inventory_integrity
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_gop2_reference_inventory.py")
endif()

# Phase-B oracle contract: missing stages/neutrality evidence never pass.
if(Python3_Interpreter_FOUND)
    add_test(NAME gop2_original_stage_contract
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_gop2_stage_contract.py")
endif()
