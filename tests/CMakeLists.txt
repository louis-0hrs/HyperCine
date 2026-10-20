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

# RCA-195 Milestone 1: re-derive the band-0 publication contract from the frozen
# SDK source. Source reading only -- this test never decodes and never writes to
# upstream/. It fails closed if a macro, guard, writer lock or band-count entry
# moves, so the published dependency map cannot silently drift from the tree.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME rca195_band0_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/verify_rca195_band0_contract.py"
        --check --repo "${CMAKE_SOURCE_DIR}")
    add_test(NAME rca195_band0_contract_selftest COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/verify_rca195_band0_contract.py"
        --self-test --repo "${CMAKE_SOURCE_DIR}")
    add_test(NAME rca195_tsan_reduction_selftest COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/analyze_rca195_tsan_log.py"
        --self-test --repo "${CMAKE_SOURCE_DIR}")
    # The probe is applied to a scratch copy only; this test proves the anchors
    # still land and that a removed anchor fails closed.
    add_test(NAME rca195_probe_applier_selftest COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/apply_rca195_probe.py"
        --self-test --repo "${CMAKE_SOURCE_DIR}")
    # The runner's own verdict logic must be able to return INCONCLUSIVE, and it
    # must accept the exact queue-line format the probe header emits.
    add_test(NAME rca195_probe_runner_selftest COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/run_rca195_probe.py"
        --self-test)
    # A published ledger has to reproduce from its own records. The superseded
    # v2 ledger does not, and this test is what makes that visible instead of
    # leaving it to a reader to notice.
    add_test(NAME rca195_ledger_rescore_v3 COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/run_rca195_probe.py"
        --rescore "${CMAKE_SOURCE_DIR}/docs/validation/rca195_probe_ledger_2026-10-20_v3.json")
    # Milestone-2 design model. Three gates, deliberately split:
    #   --check              pins every claim to the frozen tree and must stay fast
    #                        (it is an anchor check, not a search);
    #   --verdict-regression checks that the *verdict function itself* can fail,
    #                        which is cheap (no BFS) and catches the rev-5 bug
    #                        where --json discarded negative-control failures;
    #   --self-test          runs the bounded state-space sweeps. Where a space is
    #                        too large it reports INCONCLUSIVE rather than passing
    #                        itself off as a proof, so this is the slower gate.
    add_test(NAME rca195_m2_model_selftest COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/rca195_m2_model.py" --self-test)
    add_test(NAME rca195_m2_model_check COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/rca195_m2_model.py" --check)
    add_test(NAME rca195_m2_model_verdict COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/rca195_m2_model.py" --verdict-regression)
    # Scratch-SDK contract candidate. The applier is applied to a private
    # out-of-repo copy only; this test proves its anchors still land and that a
    # removed anchor fails closed instead of producing an uninstrumented build.
    add_test(NAME rca195_contract_applier_selftest COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/apply_rca195_contract.py"
        --self-test --repo "${CMAKE_SOURCE_DIR}")
    # The runner's verdict logic must fail the shapes its rules name: a broken
    # control, a surviving injected OKAY, an unattributed failure, a hang, a
    # dirty guard band and a changed control digest.
    add_test(NAME rca195_contract_runner_selftest COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/run_rca195_contract.py" --self-test)
    set_tests_properties(rca195_band0_contract rca195_band0_contract_selftest
        rca195_tsan_reduction_selftest rca195_probe_applier_selftest
        rca195_probe_runner_selftest rca195_ledger_rescore_v3
        rca195_m2_model_selftest rca195_m2_model_check
        rca195_m2_model_verdict rca195_contract_applier_selftest
        rca195_contract_runner_selftest
        PROPERTIES TIMEOUT 300
        PASS_REGULAR_EXPRESSION "VERIFY PASS;SELF-TEST PASS;RESCORE PASS;ANCHOR PASS;VERDICT-REGRESSION PASS;MODEL PASS;NEGATIVE-CASES PASS_EXHAUSTIVE;RUNNER SELF-TEST PASS")
    # The partial-order sweeps take minutes on an idle host and longer under
    # CI load; the anchor gate above stays in the shared 300 s budget.
    set_tests_properties(rca195_m2_model_selftest PROPERTIES TIMEOUT 900)
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
