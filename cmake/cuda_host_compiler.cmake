# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.

# Refuse a CUDA configuration whose C++ front end is newer than the host
# compiler nvcc links with.
#
# nvcc publishes its own host compiler's library directories, and CMake puts
# them in front of the C++ driver's search path (CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES
# ahead of the GCC installation directory). `-lstdc++` therefore resolves to the
# older release. Objects the newer front end emitted reference symbols that only
# exist there: GCC 14 emits __cxa_call_terminate (CXXABI_1.3.15), GCC 13's
# libstdc++ does not export it. The link then fails with
#
#     undefined reference to `__cxa_call_terminate'
#
# and it fails late, in whichever target links first, after the whole tree has
# compiled. Nothing in HyperCine's sources causes this and no source change can
# fix it: the two toolchains in one configure are simply not linkable. The
# configuration is therefore refused here, before the build starts, rather than
# being attempted and reported as a source defect.
#
# Scope is deliberately narrow. Only the GNU/GNU pair is judged, and only the
# direction that was measured. A newer nvcc host compiler than the C++ front end
# is not covered: nvcc itself rejects unsupported host compiler versions during
# language enablement, so that configuration never reaches this function.
function(hypercine_require_consistent_cuda_host_compiler
         cxx_id cxx_version host_id host_version host_compiler)
    if(NOT cxx_id STREQUAL "GNU" OR NOT host_id STREQUAL "GNU")
        return()
    endif()
    if(cxx_version STREQUAL "" OR host_version STREQUAL "")
        # Detection did not report a version. A refusal would be a guess.
        return()
    endif()

    string(REGEX MATCH "^[0-9]+" cxx_major "${cxx_version}")
    string(REGEX MATCH "^[0-9]+" host_major "${host_version}")
    if(cxx_major STREQUAL "" OR host_major STREQUAL "")
        return()
    endif()

    if(cxx_major GREATER host_major)
        message(FATAL_ERROR
            "CUDA host compiler mismatch: CMAKE_CXX_COMPILER is GNU ${cxx_version} "
            "but nvcc links with GNU ${host_version} "
            "(${host_compiler}).\n"
            "nvcc puts its host compiler's library directory ahead of the C++ "
            "driver's, so `-lstdc++` resolves to the older release and objects "
            "emitted by ${cxx_version} fail to link with "
            "'undefined reference to `__cxa_call_terminate''. This is a toolchain "
            "mismatch, not a source defect, and it cannot be fixed from this "
            "repository.\n"
            "Configure with one consistent host compiler, for example:\n"
            "  -DCMAKE_C_COMPILER=gcc-${host_major} "
            "-DCMAKE_CXX_COMPILER=g++-${host_major}\n"
            "and, when nvcc's own default host compiler is not that version, "
            "name it through the environment variable CMake reads during "
            "CUDA language enablement:\n"
            "  CUDAHOSTCXX=g++-${host_major} cmake ...\n"
            "Note that -DCMAKE_CUDA_HOST_COMPILER does NOT do this here: it is "
            "recorded in the cache but never reaches nvcc, so the reported "
            "host compiler stays whatever the toolkit defaults to. Measured "
            "on CMake 3.31.6 with nvcc 12.4.131, where even a nonexistent path "
            "in that variable configures successfully. Use CUDAHOSTCXX.")
    endif()
endfunction()
