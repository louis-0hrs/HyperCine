// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// RCA-195 scratch contract: first-error precedence regression.
//
// A reviewer found that the first R6b guard
//
//     if (error != CODEC_ERROR_OKAY || decoder->error == CODEC_ERROR_OKAY)
//         decoder->error = error;
//
// stops OKAY from erasing a failure, but is NOT first-error semantics: when both
// values are non-OKAY the later one replaces the earlier one, so the reported
// code depends on arrival order. That is a real behavioural difference, not a
// documentation gap, so it gets a test rather than a caveat.
//
// This test exercises the actual macro from tools/rca195_contract.h. It links
// nothing else: the contract header is self-contained, and the owning-translation
// unit define gives it its own copy of the counters.
#define RCA195_PROBE_OWNER 1
#include "rca195_contract.h"

/* The real enum, so the assertions are pinned to the frozen decoder's numbers
 * rather than to literals the test invents for itself. */
#include "error.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void expect(const char *what, int got, int want)
{
	if (got == want)
		return;
	std::fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
	++failures;
}

// The ordinals this test names, pinned to upstream/Codec/error.h so a renumbering
// of the enum cannot silently turn these assertions into aliases.
const int OKAY = CODEC_ERROR_OKAY;             // 0
const int BAD_FRAME = CODEC_ERROR_BAD_FRAME;   // 58
const int RESOLUTION = CODEC_ERROR_RESOLUTION; // 36

// Pinned independently of the enum above: if upstream renumbers, this fails
// loudly instead of quietly testing aliases.
static_assert(CODEC_ERROR_OKAY == 0, "CODEC_ERROR_OKAY moved");
static_assert(CODEC_ERROR_BAD_FRAME == 58, "CODEC_ERROR_BAD_FRAME moved");
static_assert(CODEC_ERROR_RESOLUTION == 36, "CODEC_ERROR_RESOLUTION moved");

}  // namespace

int main()
{
	// 1. OKAY never displaces a code.
	{
		CODEC_ERROR field = static_cast<CODEC_ERROR>(BAD_FRAME);
		RCA195_CONTRACT_KEEP_ERROR(field, CODEC_ERROR_OKAY);
		expect("OKAY does not erase a real error", (int)field, BAD_FRAME);
	}

	// 2. An empty field accepts the first failure.
	{
		CODEC_ERROR field = CODEC_ERROR_OKAY;
		RCA195_CONTRACT_KEEP_ERROR(field, static_cast<CODEC_ERROR>(BAD_FRAME));
		expect("first failure is recorded", (int)field, BAD_FRAME);
	}

	// 3. The first non-OKAY code is sticky: a later, different code does not
	//    replace it. This is the case the reviewer named -- under the previous
	//    guard the field would become RESOLUTION here.
	{
		unsigned long long before = rca195_contract_precedence;
		CODEC_ERROR field = static_cast<CODEC_ERROR>(BAD_FRAME);
		RCA195_CONTRACT_KEEP_ERROR(field, static_cast<CODEC_ERROR>(RESOLUTION));
		expect("earlier failure survives a later one", (int)field, BAD_FRAME);
		expect("precedence conflict is counted", 
			   (int)(rca195_contract_precedence - before), 1);
	}

	// 4. An identical second code is not a conflict.
	{
		unsigned long long before = rca195_contract_precedence;
		CODEC_ERROR field = static_cast<CODEC_ERROR>(BAD_FRAME);
		RCA195_CONTRACT_KEEP_ERROR(field, static_cast<CODEC_ERROR>(BAD_FRAME));
		expect("same code keeps the field", (int)field, BAD_FRAME);
		expect("same code is not a conflict",
			   (int)(rca195_contract_precedence - before), 0);
	}

	// 5. OKAY into OKAY stays OKAY and is not a conflict.
	{
		unsigned long long before = rca195_contract_precedence;
		CODEC_ERROR field = CODEC_ERROR_OKAY;
		RCA195_CONTRACT_KEEP_ERROR(field, CODEC_ERROR_OKAY);
		expect("OKAY stays OKAY", (int)field, OKAY);
		expect("OKAY is not a conflict",
			   (int)(rca195_contract_precedence - before), 0);
	}

	// 6. Order independence: the surviving code is the same whichever order the
	//    two failures arrive in. This is the property the old guard lacked.
	{
		CODEC_ERROR a = CODEC_ERROR_OKAY;
		RCA195_CONTRACT_KEEP_ERROR(a, static_cast<CODEC_ERROR>(BAD_FRAME));
		RCA195_CONTRACT_KEEP_ERROR(a, static_cast<CODEC_ERROR>(RESOLUTION));

		CODEC_ERROR b = CODEC_ERROR_OKAY;
		RCA195_CONTRACT_KEEP_ERROR(b, static_cast<CODEC_ERROR>(RESOLUTION));
		RCA195_CONTRACT_KEEP_ERROR(b, static_cast<CODEC_ERROR>(BAD_FRAME));

		expect("first failure wins in order A then B", (int)a, BAD_FRAME);
		expect("first failure wins in order B then A", (int)b, RESOLUTION);
	}

	if (failures) {
		std::fprintf(stderr, "ERROR-PRECEDENCE FAIL: %d case(s)\n", failures);
		return 1;
	}
	std::printf("ERROR-PRECEDENCE PASS: 6 cases, first non-OKAY code is sticky "
				"and OKAY never displaces it\n");
	return 0;
}
