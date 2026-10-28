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
    # Shared with the standalone build, which never reaches this file.
    hypercine_register_sdk_free_contracts()
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
endif()

# Issue #253: every target below links the frozen `CFHDCodecStatic` archive,
# which only the upstream build defines. With -DHYPERCINE_BUILD_UPSTREAM=OFF an
# unguarded link reached the linker as `cannot find -lCFHDCodecStatic` instead
# of a truthful configuration, and the only tested configuration was the one
# (standalone) that returns before this file. The SDK-free contracts above keep
# building; the SDK-backed targets are guarded on the target and the omission is
# reported at configure time instead of being silently unregistered.
set(HYPERCINE_SDK_TESTS_OMITTED "")
macro(hypercine_test_needs_sdk)
    list(APPEND HYPERCINE_SDK_TESTS_OMITTED ${ARGN})
endmacro()

# Bounded Windows original-SDK process probe (not a pixel-correctness oracle).
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET CFHDCodecStatic)
    add_executable(windows_reference_process windows_reference_process.cpp)
    target_link_libraries(windows_reference_process PRIVATE CFHDCodecStatic)
endif()

# 1. Transform-level test
if(WIN32 AND TARGET CFHDCodecStatic)
    add_executable(decoder_sdk_dump decoder_differential/sdk_dump.cpp)
    target_link_libraries(decoder_sdk_dump PRIVATE CFHDCodecStatic)
    add_test(NAME decoder_differential_yu64_unpack COMMAND decoder_sdk_dump --self-test)
endif()

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
elseif(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    hypercine_test_needs_sdk(test_small_chroma_oracle)
endif()

add_executable(test_transform_wavelet
    test_transform_wavelet.cpp
    ${CMAKE_SOURCE_DIR}/upstream/Example/WaveletDemo/utils.c
)
add_test(NAME test_transform_wavelet COMMAND test_transform_wavelet)

# 2. Codec-level roundtrip test
if(TARGET CFHDCodecStatic)
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
else()
    # test_transform_wavelet above deliberately stays: it compiles the frozen
    # WaveletDemo helper sources but links no SDK archive.
    hypercine_test_needs_sdk(
        test_codec_roundtrip test_yuy2_stability test_avi_sample)
endif()

# 4. Phase 1A CUDA wavelet transform test
if(HYPERCINE_ENABLE_CUDA)
    add_executable(test_cuda_wavelet
        test_cuda_wavelet.cpp
    )
    target_link_libraries(test_cuda_wavelet PRIVATE hypercine_core)
    add_test(NAME test_cuda_wavelet COMMAND test_cuda_wavelet)
endif()


if(HYPERCINE_ENABLE_CUDA AND TARGET CFHDCodecStatic)
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
elseif(HYPERCINE_ENABLE_CUDA)
    # CUDA is on but the frozen SDK is not built, so every CUDA test below would
    # only fail at link time. Keep test_cuda_wavelet (hypercine_core only) and
    # report the archive-dependent ones explicitly.
    hypercine_test_needs_sdk(
        test_cuda_pyramid test_entropy_parallel test_hybrid_roundtrip
        test_phase2a test_phase2b test_phase2c test_phase2d
        test_entropy_capacity test_phase3a)
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
    # Exit 77 means every strict signal (fixture bytes, decode status,
    # geometry, pitch, non-zero output) passed but repeated decodes of the same
    # packet disagreed. That is the unknown reference state, not a failure.
    set_tests_properties(test_filmscan3_windows_decoder PROPERTIES
        DEPENDS filmscan3_fixture_integrity
        SKIP_RETURN_CODE 77
        LABELS "reference-oracle"
    )
endif()

if(HYPERCINE_ENABLE_CUDA AND WIN32)
    add_executable(hypercine_reference_export ../tools/reference_corpus_export.cpp)
    target_link_libraries(hypercine_reference_export PRIVATE hypercine_wavelet CFHDCodecStatic bcrypt)
    target_compile_definitions(hypercine_reference_export PRIVATE
        HYPERCINE_SOURCE_COMMIT="${HYPERCINE_SOURCE_COMMIT}"
        HYPERCINE_SOURCE_DIRTY=${HYPERCINE_SOURCE_DIRTY}
        HYPERCINE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
endif()

# Issue #36 stage F0: real GoPro SDK capability probe over every compressed
# CineForm family. Isolated tool; it reuses no existing corpus export contract
# and changes no existing schema. Run it manually on a host with the SDK build.
if(WIN32 AND TARGET CFHDCodecStatic)
    add_executable(format_corpus_probe ../tools/format_corpus_probe.cpp)
    target_link_libraries(format_corpus_probe PRIVATE CFHDCodecStatic)
    add_executable(test_format_corpus_probe test_format_corpus_probe.cpp)
    target_link_libraries(test_format_corpus_probe PRIVATE CFHDCodecStatic)
    add_test(NAME format_corpus_probe_faults COMMAND test_format_corpus_probe)
    if(MSVC)
        # The pristine sample layer throws CFHD_ERROR_BADFORMAT out of extern "C"
        # entry points such as CFHD_GetImagePitch (see docs/UPSTREAM.md). MSVC's
        # default /EHsc lets the compiler assume extern "C" functions never throw,
        # which makes that unwind undefined behaviour and crashes Release builds.
        # /EHs drops the assumption. MSVC reports D9025 for overriding /EHsc.
        target_compile_options(format_corpus_probe PRIVATE /EHs)
        target_compile_options(test_format_corpus_probe PRIVATE /EHs)
    endif()
    # Cheap guard for the probe itself: one family and one source format must still
    # encode and decode all six qualities. The full matrix stays an on-demand run.
    add_test(NAME format_corpus_probe_smoke COMMAND format_corpus_probe
        --family YUV_422 --source BGRA --width 256 --height 256)
    set_tests_properties(format_corpus_probe_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "total=6 bidirectional_pass=6"
        TIMEOUT 900)
endif()

# Issue #36 stage F1: real SDK golden corpora. The full matrix is an on-demand
# run against a corpus directory outside Git; CTest only writes one tiny case
# into the build tree and verifies its manifest.
if(WIN32 AND TARGET CFHDCodecStatic)
    add_executable(format_corpus_export ../tools/format_corpus_export.cpp)
    target_link_libraries(format_corpus_export PRIVATE CFHDCodecStatic bcrypt)
    add_executable(test_format_corpus_export test_format_corpus_export.cpp)
    target_link_libraries(test_format_corpus_export PRIVATE bcrypt)
    add_test(NAME format_corpus_export_helpers COMMAND test_format_corpus_export)
    if(MSVC)
        target_compile_options(format_corpus_export PRIVATE /EHs)
    endif()
    set(f1_smoke_dir "${CMAKE_CURRENT_BINARY_DIR}/f1-smoke")
    add_test(NAME format_corpus_export_smoke COMMAND format_corpus_export
        --out "${f1_smoke_dir}" --width 128 --height 128
        --family RGB_444 --source RG48 --pattern asymmetric --quality filmscan1)
    set_tests_properties(format_corpus_export_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "exported=1 encode_failed=0 decode_failed=0 write_failed=0"
        TIMEOUT 600)
    add_test(NAME format_corpus_export_smoke_verify COMMAND format_corpus_export
        --verify "${f1_smoke_dir}")
    set_tests_properties(format_corpus_export_smoke_verify PROPERTIES
        PASS_REGULAR_EXPRESSION "Verified [1-9][0-9]* manifest artifacts"
        DEPENDS format_corpus_export_smoke
        TIMEOUT 120)
    # Tier labels are metadata only; these checks never allocate a frame.
    add_test(NAME format_corpus_export_tier_list COMMAND format_corpus_export
        --list --tier f2 --width 3840 --height 2160
        --family RGB_444 --source RG48 --pattern asymmetric --quality filmscan3)
    set_tests_properties(format_corpus_export_tier_list PROPERTIES
        PASS_REGULAR_EXPRESSION "RGB_444/f2/RG48/filmscan3/asymmetric/3840x2160"
        TIMEOUT 60)
    add_test(NAME format_corpus_export_tier_invalid COMMAND format_corpus_export
        --out "${CMAKE_CURRENT_BINARY_DIR}/tier-invalid" --tier f3)
    set_tests_properties(format_corpus_export_tier_invalid PROPERTIES
        WILL_FAIL TRUE
        TIMEOUT 60)
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
    # Exit 77 means every strict storage/decoder/encoder/geometry/pitch/non-zero
    # signal passed, but the decoded-pixel comparison against the pinned reference
    # is BLOCKED_ORACLE (reference not independently qualified) or independent
    # decodes of the same immutable packet disagreed. Neither is a pixel-accuracy
    # pass, so there is no PASS output line to match any more.
    set_tests_properties(standalone_sdk_decode_3_packets PROPERTIES
        DEPENDS standalone_oracle_fixture_hashes TIMEOUT 120
        SKIP_RETURN_CODE 77
        LABELS "reference-oracle"
        FAIL_REGULAR_EXPRESSION "STRICT_FAILURE;Oracle harness error")
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

if(HYPERCINE_SDK_TESTS_OMITTED)
    list(REMOVE_DUPLICATES HYPERCINE_SDK_TESTS_OMITTED)
    list(LENGTH HYPERCINE_SDK_TESTS_OMITTED HYPERCINE_SDK_TESTS_OMITTED_COUNT)
    message(STATUS
        "HYPERCINE_BUILD_UPSTREAM=OFF: ${HYPERCINE_SDK_TESTS_OMITTED_COUNT} SDK-backed "
        "test target(s) are unavailable and were not registered (they link the "
        "frozen CFHDCodecStatic archive): ${HYPERCINE_SDK_TESTS_OMITTED}")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME gop2_reference_inventory_integrity
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_gop2_reference_inventory.py")
endif()

# Issue #253 contract: BUILD_UPSTREAM=OFF configures and reports its omission
# instead of failing at link time. Configure-only; runs a fresh child configure
# in both directions so a vacuous guard fails too.
if(Python3_Interpreter_FOUND)
    add_test(NAME upstream_off_configure_contract
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/test_upstream_off_contract.py")
    set_tests_properties(upstream_off_configure_contract PROPERTIES
        LABELS "build-contract" TIMEOUT 900)
endif()

# Phase-B oracle contract: missing stages/neutrality evidence never pass.
if(Python3_Interpreter_FOUND)
    add_test(NAME gop2_original_stage_contract
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_gop2_stage_contract.py")
endif()

# Candidate independent pixel oracle: external FFmpeg, explicitly version-bound.
# A missing tool or an unmeasured build exits 77 and never reports qualified.
if(Python3_Interpreter_FOUND)
    add_test(NAME ffmpeg_oracle_qualification
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/qualify_ffmpeg_oracle.py")
    set_tests_properties(ffmpeg_oracle_qualification PROPERTIES
        LABELS "reference-oracle" SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_test(NAME ffmpeg_oracle_qualification_contract
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/test_ffmpeg_oracle_qualification.py")
    set_tests_properties(ffmpeg_oracle_qualification_contract PROPERTIES
        LABELS "reference-oracle")
endif()
