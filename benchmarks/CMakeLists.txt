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
