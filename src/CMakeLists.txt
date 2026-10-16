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
    )
    target_include_directories(hypercine_wavelet PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
endif()

target_link_libraries(hypercine_wavelet PUBLIC hypercine_quality_state)

if(TARGET hypercine_core)
    target_link_libraries(hypercine_core INTERFACE hypercine_wavelet)
endif()
