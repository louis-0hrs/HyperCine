# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
cmake_minimum_required(VERSION 3.25)
file(READ "${ROOT}/tests/fixtures/macos-filmscan3/manifest.json" manifest)
foreach(i RANGE 0 1)
    string(JSON name GET "${manifest}" samples ${i} file)
    string(JSON expected GET "${manifest}" samples ${i} sha256)
    string(JSON length GET "${manifest}" samples ${i} byte_length)
    set(path "${ROOT}/tests/fixtures/macos-filmscan3/${name}")
    file(SHA256 "${path}" actual)
    file(SIZE "${path}" size)
    if(NOT actual STREQUAL expected OR NOT size EQUAL length)
        message(FATAL_ERROR "Original Mac-produced FS3 fixture failed SHA/length verification: ${name}")
    endif()
endforeach()
