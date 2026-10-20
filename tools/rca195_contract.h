// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// RCA-195 Milestone-2 scratch-SDK contract candidate. TEST-ONLY: applied to a
// private out-of-repo SDK copy by tools/apply_rca195_contract.py and never part
// of the frozen upstream tree.
//
// Why pointer identity instead of a "generation" number
// ------------------------------------------------------
// The frozen decoder has no generation counter. `grep -rni generation Codec/*.c`
// returns two unrelated comments about multi-generation encodes and nothing
// else. The modelled "generation" therefore has to map onto something real, and
// the real stale token is the wavelet pointer: GetWaveletThreadSafe ->
// ReallocWaveletEx deletes and recreates the IMAGE whenever the requested
// dimensions change, and CreateWaveletEx clears band_valid_flags. A queue entry
// that still holds the old pointer holds a token for an object that no longer
// exists -- exactly the state R3/R5 are supposed to reject.
//
// This is a *candidate*, not a fix. It proves the contract can be implemented
// and that its controls separate; it does not prove the scheduler safe.
#ifndef HYPERCINE_RCA195_CONTRACT_H
#define HYPERCINE_RCA195_CONTRACT_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* One definition, in the unit that defines RCA195_PROBE_OWNER (decoder.c).
 * Without this the counters would be per-translation-unit copies and the drain
 * check would read zero while the worker incremented its own copy. */
#ifdef RCA195_PROBE_OWNER
#define RCA195_CONTRACT_STATE
#else
#define RCA195_CONTRACT_STATE extern
#endif

RCA195_CONTRACT_STATE FILE *rca195_contract_log;
RCA195_CONTRACT_STATE int rca195_contract_ready;
RCA195_CONTRACT_STATE int rca195_contract_skip_ch;
RCA195_CONTRACT_STATE int rca195_contract_skip_idx;
RCA195_CONTRACT_STATE int rca195_contract_stale_ch;
RCA195_CONTRACT_STATE int rca195_contract_stale_idx;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_claims;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_executed;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_stale;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_drain_short;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_skip;

static void RCA195ContractInit(void)
{
	const char *path;
	const char *skip;

	if (rca195_contract_ready)
		return;
	rca195_contract_ready = 1;
	rca195_contract_skip_ch = -1;
	rca195_contract_skip_idx = -1;
	rca195_contract_stale_ch = -1;
	rca195_contract_stale_idx = -1;

	path = getenv("RCA195_CONTRACT_LOG");
	if (path != NULL && path[0] != '\0')
		rca195_contract_log = fopen(path, "a");

	/* "<channel>:<index>" -- consume the admitted entry, never execute it. */
	skip = getenv("RCA195_CONTRACT_SKIP");
	if (skip != NULL && skip[0] != '\0')
	{
		int ch = -1, idx = -1;
		if (sscanf(skip, "%d:%d", &ch, &idx) == 2)
		{
			rca195_contract_skip_ch = ch;
			rca195_contract_skip_idx = idx;
		}
	}
	/* "<channel>:<index>" -- force the R3/R5 token mismatch on one target so
	 * the fail-closed path is measured rather than assumed unreachable. */
	skip = getenv("RCA195_CONTRACT_STALE");
	if (skip != NULL && skip[0] != '\0')
	{
		int ch = -1, idx = -1;
		if (sscanf(skip, "%d:%d", &ch, &idx) == 2)
		{
			rca195_contract_stale_ch = ch;
			rca195_contract_stale_idx = idx;
		}
	}
}

static void RCA195ContractLog(const char *fmt, ...)
{
	va_list args;

	RCA195ContractInit();
	if (rca195_contract_log == NULL)
		return;
	va_start(args, fmt);
	vfprintf(rca195_contract_log, fmt, args);
	va_end(args);
	fputc('\n', rca195_contract_log);
	fflush(rca195_contract_log);
}

static int RCA195ContractShouldSkip(int channel, int index)
{
	RCA195ContractInit();
	if (rca195_contract_skip_ch < 0)
		return 0;
	if (channel != rca195_contract_skip_ch || index != rca195_contract_skip_idx)
		return 0;
	rca195_contract_skip++;
	return 1;
}

/* Returns 1 when the R3/R5 re-check must refuse this target. Injected failure:
 * used only to prove the check fires. The real trigger is a reallocation in the
 * window between claim and execution. */
static int RCA195ContractForceStale(int channel, int index)
{
	RCA195ContractInit();
	if (rca195_contract_stale_ch < 0)
		return 0;
	return channel == rca195_contract_stale_ch &&
		   index == rca195_contract_stale_idx;
}

static void RCA195ContractClaim(int channel, int index)
{
	RCA195ContractInit();
	/* The counters are touched from worker threads. A plain ++ loses updates
	 * under contention, which would make `claims != executed` fire on a valid
	 * decode and blame the contract for the instrument. Relaxed atomics are
	 * enough: the values are read after the pool has drained. */
	__atomic_fetch_add(&rca195_contract_claims, 1, __ATOMIC_RELAXED);
	RCA195ContractLog("CLAIM ch=%d idx=%d", channel, index);
}

static void RCA195ContractExecute(int channel, int index)
{
	RCA195ContractInit();
	__atomic_fetch_add(&rca195_contract_executed, 1, __ATOMIC_RELAXED);
	RCA195ContractLog("EXEC ch=%d idx=%d", channel, index);
}

/* R3/R5: the entry survived the queue lock, but its token no longer names the
 * live wavelet. Terminal, never retried, never executed. */
static void RCA195ContractStale(int channel, int index)
{
	RCA195ContractInit();
	__atomic_fetch_add(&rca195_contract_stale, 1, __ATOMIC_RELAXED);
	RCA195ContractLog("STALE ch=%d idx=%d", channel, index);
}

/* R6: the drain is about to discard work that was never completed. */
static void RCA195ContractDrainShort(int remaining, unsigned long long claimed,
									 unsigned long long executed)
{
	RCA195ContractInit();
	__atomic_fetch_add(&rca195_contract_drain_short, 1, __ATOMIC_RELAXED);
	RCA195ContractLog("DRAIN_SHORT remaining=%d claimed=%llu executed=%llu",
					  remaining, claimed, executed);
}

/* No RESULT line: every call already owns its own log file, so the DRAIN line
 * is the per-call terminator. An earlier draft carried a result hook with no
 * call site -- dead instrumentation that looked like a capability, which is the
 * defect this project already removed once. */

#endif /* HYPERCINE_RCA195_CONTRACT_H */
