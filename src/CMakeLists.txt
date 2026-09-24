# HyperCine Source Library CMakeLists.txt

if(HYPERCINE_ENABLE_CUDA)
    add_library(hypercine_wavelet STATIC
        cpu/wavelet_cpu.cpp
        cuda/wavelet_cuda_reference.cu
        cuda/wavelet_cuda_optimized.cu
        cuda/wavelet_cuda_api.cu
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
        cuda/wavelet_cuda_stub.cpp
    )
    target_include_directories(hypercine_wavelet PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
endif()

if(TARGET hypercine_core)
    target_link_libraries(hypercine_core INTERFACE hypercine_wavelet)
endif()
