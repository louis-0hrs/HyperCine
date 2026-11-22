# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.

function(hypercine_require_vc5_contract_files source_root)
    foreach(required_file IN ITEMS
            tests/test_vc5_inspect.py tests/test_vc5_inspect_mutations.py
            tests/test_vc5_synthetic_ledger.py tools/vc5_inspect.py
            tools/vc5_synthetic_packet.py)
        if(NOT EXISTS "${source_root}/${required_file}" OR
           IS_DIRECTORY "${source_root}/${required_file}")
            message(FATAL_ERROR "Required VC-5 contract source missing: ${required_file}")
        endif()
    endforeach()
endfunction()
