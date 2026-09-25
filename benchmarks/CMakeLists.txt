# HyperCine Benchmark Suite CMakeLists.txt

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/upstream/Common
    ${CMAKE_SOURCE_DIR}/upstream/Codec
    ${CMAKE_SOURCE_DIR}/upstream/EncoderSDK
    ${CMAKE_SOURCE_DIR}/upstream/DecoderSDK
)

add_executable(benchmark_codec
    benchmark_codec.cpp
)

target_link_libraries(benchmark_codec PRIVATE CFHDCodecStatic)

if(WIN32)
    target_link_libraries(benchmark_codec PRIVATE psapi)
endif()

if(HYPERCINE_ENABLE_CUDA)
    add_executable(benchmark_wavelet
        benchmark_wavelet.cpp
    )
    target_link_libraries(benchmark_wavelet PRIVATE hypercine_core)
    
    add_executable(benchmark_phase1b
        benchmark_phase1b.cpp
    )
    target_link_libraries(benchmark_phase1b PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(profile_encoder
        profile_encoder.cpp
    )
    target_link_libraries(profile_encoder PRIVATE hypercine_core CFHDCodecStatic)

    if(WIN32)
        target_link_libraries(benchmark_wavelet PRIVATE psapi)
        target_link_libraries(benchmark_phase1b PRIVATE psapi)
        target_link_libraries(profile_encoder PRIVATE psapi)
    endif()
endif()

