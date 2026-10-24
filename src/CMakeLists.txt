# HyperCine Source Library CMakeLists.txt

if(HYPERCINE_ENABLE_CUDA)
    add_library(hypercine_wavelet STATIC
        cpu/wavelet_cpu.cpp
        cpu/upstream_wrapper.cpp
        cpu/entropy_parallel.cpp
        cpu/entropy_stages.cpp
        core/hybrid_encoder.cpp
        core/phase2a_encoder.cpp
        core/phase2b_encoder.cpp
        core/phase2c_encoder.cpp
        core/phase2d_encoder.cpp
        core/phase3a_encoder.cpp
        cuda/wavelet_cuda_reference.cu
        cuda/wavelet_cuda_optimized.cu
        cuda/wavelet_cuda_api.cu
        cuda/hybrid_kernels.cu
        cuda/phase2a_transfer.cu
        cuda/entropy_gpu.cu
        cuda/decode_inverse_cuda.cu
        cuda/decode_color_cuda.cu
        cuda/decode_phase2a_pipeline.cu
        core/decode_parser.cpp
        core/decode_fieldplus_parser.cpp
        core/decode_fsm_tables.cpp
        core/decode_thread_pool.cpp
        cuda/decode_entropy_cuda.cu
        cuda/decode_gpu_index.cu
        cuda/decode_phase2b_pipeline.cu
        cuda/decode_phase2c_pipeline.cu
        cuda/decode_phase2d_pipeline.cu
        cuda/decode_phase2e_pipeline.cu
        cuda/decode_hybrid_entropy.cu
        cuda/decode_peaks_cuda.cu
    )
    set_target_properties(hypercine_wavelet PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_SEPARABLE_COMPILATION OFF
    )
    target_include_directories(hypercine_wavelet PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/cuda
    )
    target_compile_definitions(hypercine_wavelet PUBLIC HYPERCINE_CUDA_AVAILABLE=1)
else()
    add_library(hypercine_wavelet STATIC
        cpu/wavelet_cpu.cpp
        cpu/upstream_wrapper.cpp
        cuda/wavelet_cuda_stub.cpp
        cuda/decode_inverse_stub.cpp
        core/decode_parser.cpp
        core/decode_fieldplus_parser.cpp
        core/decode_thread_pool.cpp
        cuda/decode_phase2b_stub.cpp
        cuda/decode_phase2c_stub.cpp
        cuda/decode_phase2d_stub.cpp
        cuda/decode_phase2e_stub.cpp
        cuda/decode_gpu_index_stub.cpp
    )
    target_include_directories(hypercine_wavelet PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
endif()

# Stable decoder facade. Historical entry points remain in the implementation
# archive; consumers include decoder.h and need no research declarations.
add_library(hypercine_decoder STATIC core/decoder.cpp core/decode_dispatch.cpp)
target_include_directories(hypercine_decoder PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(hypercine_decoder PRIVATE hypercine_wavelet)
if(HYPERCINE_ENABLE_CUDA)
    target_include_directories(hypercine_decoder PRIVATE ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
endif()
add_library(HyperCine::Decoder ALIAS hypercine_decoder)

# hypercine_wavelet calls the upstream CFHD entry points. Declaring the dependency
# keeps the link line ordered (hypercine_wavelet before CFHDCodecStatic) on
# toolchains whose static archive resolution is a single left-to-right pass (GNU ld).
if(TARGET CFHDCodecStatic)
    target_link_libraries(hypercine_wavelet PUBLIC CFHDCodecStatic)
endif()

if(TARGET hypercine_core)
    target_link_libraries(hypercine_core INTERFACE hypercine_wavelet)
endif()
