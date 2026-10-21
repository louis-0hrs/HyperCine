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
RCA195_CONTRACT_STATE unsigned long long rca195_contract_precedence;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_wfree;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_walloc;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_aba;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_realloc_calls;
RCA195_CONTRACT_STATE unsigned long long rca195_contract_realloc_early;

/* Recently freed wavelet addresses, so address reuse is measurable instead of
 * argued about. Small on purpose: reuse inside the claim->execute window is what
 * matters, and that window spans a handful of allocations. */
#define RCA195_CONTRACT_ABA_RING 16
RCA195_CONTRACT_STATE void *rca195_contract_freed[RCA195_CONTRACT_ABA_RING];
RCA195_CONTRACT_STATE unsigned int rca195_contract_freed_at;

/* Portable increment.
 *
 * `__atomic_fetch_add` is a GCC/Clang builtin and does not exist under MSVC, so
 * a header carrying it breaks the Windows build -- which is how this was found:
 * the precedence test includes this header, and the MSVC job failed on ten uses
 * of an undeclared identifier. The probe only needs "no lost update from worker
 * threads", not a particular memory order, so a relaxed increment is enough on
 * every compiler. MSVC gets InterlockedIncrement64; everything else keeps the
 * builtin. */
#if defined(_MSC_VER)
#include <intrin.h>
#define RCA195_CONTRACT_INC(counter)                                          \
	(void)InterlockedIncrement64((volatile __int64 *)&(counter))
#else
#define RCA195_CONTRACT_INC(counter)                                          \
	__atomic_fetch_add(&(counter), 1, __ATOMIC_RELAXED)
#endif

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
	FILE *f;

	RCA195ContractInit();
	f = rca195_contract_log;
	if (f == NULL)
		return;

	/* One line per call, atomically. Every decoder worker shares this FILE*,
	 * and without the stdio-level lock two writers can interleave or lose a
	 * line. Measured: one call showed 38 realloc entries against 28 early
	 * returns + 9 allocations -- exactly one event missing -- and the wavelet
	 * free counter read zero in all 100 calls. A count that reads zero because
	 * its line vanished is indistinguishable from "it never happened", so the
	 * log has to be atomic before any counter here is evidence. */
#ifdef _WIN32
	_lock_file(f);
#else
	flockfile(f);
#endif
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fflush(f);
#ifdef _WIN32
	_unlock_file(f);
#else
	funlockfile(f);
#endif
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
	RCA195_CONTRACT_INC(rca195_contract_claims);
	RCA195ContractLog("CLAIM ch=%d idx=%d", channel, index);
}

static void RCA195ContractExecute(int channel, int index)
{
	RCA195ContractInit();
	RCA195_CONTRACT_INC(rca195_contract_executed);
	RCA195ContractLog("EXEC ch=%d idx=%d", channel, index);
}

/* R3/R5: the entry survived the queue lock, but its token no longer names the
 * live wavelet. Terminal, never retried, never executed. */
static void RCA195ContractStale(int channel, int index)
{
	RCA195ContractInit();
	RCA195_CONTRACT_INC(rca195_contract_stale);
	RCA195ContractLog("STALE ch=%d idx=%d", channel, index);
}

/* R6: the drain is about to discard work that was never completed. */
static void RCA195ContractDrainShort(int remaining, unsigned long long claimed,
									 unsigned long long executed)
{
	RCA195ContractInit();
	RCA195_CONTRACT_INC(rca195_contract_drain_short);
	RCA195ContractLog("DRAIN_SHORT remaining=%d claimed=%llu executed=%llu",
					  remaining, claimed, executed);
}

/* No RESULT line: every call already owns its own log file, so the DRAIN line
 * is the per-call terminator. An earlier draft carried a result hook with no
 * call site -- dead instrumentation that looked like a capability, which is the
 * defect this project already removed once. */

/* R6b -- FIRST-ERROR PRECEDENCE, not "last error wins".
 *
 * The assignment this replaces read `decoder->error = error;` with a local that
 * starts at OKAY, so a real failure recorded by the drain was erased. The first
 * revision of this guard was
 *
 *     if (error != OKAY || decoder->error == OKAY) decoder->error = error;
 *
 * which stops the erasure but is *not* first-error semantics: when both values
 * are non-OKAY the later one replaces the earlier one. A reviewer named that
 * precisely, correctly, and it is fixed here rather than documented around.
 *
 * The rule now: OKAY never displaces a code; the first non-OKAY code is sticky;
 * a second, different non-OKAY code is recorded as a precedence conflict so the
 * hierarchy is measured instead of assumed. */
static void RCA195ContractPrecedence(int kept, int dropped)
{
	RCA195ContractInit();
	RCA195_CONTRACT_INC(rca195_contract_precedence);
	RCA195ContractLog("PRECEDENCE kept=%d dropped=%d", kept, dropped);
}

/* ABA probe -- the limitation the reviewer required be made explicit.
 *
 * R3/R5 compares `transform->wavelet[index]` against the pointer captured at
 * claim time. That detects a *changed* pointer but cannot distinguish a genuine
 * reallocation from the allocator handing back the same address for a new
 * object, which would make a stale token look current. Rather than assert that
 * reuse "probably does not happen", record every wavelet free and every wavelet
 * allocation and count the collisions. If the count is non-zero on valid
 * decodes, the caveat is demonstrated rather than claimed. */
/* Reachability probe for the wavelet reallocation path.
 *
 * The first ABA run logged no free and no alloc line at all, which is exactly
 * the shape of dead instrumentation: a counter that is zero because nothing ever
 * reaches it looks identical to "no address reuse was observed". These two
 * hooks separate the possibilities -- called-but-early-returned (no
 * reallocation happened) from never-called (the probe is unreachable). */
static void RCA195ContractReallocEnter(void *p)
{
	RCA195ContractInit();
	RCA195_CONTRACT_INC(rca195_contract_realloc_calls);
	RCA195ContractLog("WREALLOC_ENTER ptr=%p", p);
}

static void RCA195ContractReallocEarly(void *p)
{
	RCA195ContractInit();
	RCA195_CONTRACT_INC(rca195_contract_realloc_early);
	RCA195ContractLog("WREALLOC_EARLY ptr=%p", p);
}

static void RCA195ContractWaveletFree(void *p)
{
	RCA195ContractInit();
	RCA195_CONTRACT_INC(rca195_contract_wfree);
	rca195_contract_freed[rca195_contract_freed_at % RCA195_CONTRACT_ABA_RING] = p;
	rca195_contract_freed_at++;
	RCA195ContractLog("WFREE ptr=%p", p);
}

static void RCA195ContractWaveletAlloc(void *p)
{
	unsigned int i;
	unsigned int span;

	RCA195ContractInit();
	RCA195_CONTRACT_INC(rca195_contract_walloc);
	RCA195ContractLog("WALLOC ptr=%p", p);

	span = rca195_contract_freed_at < RCA195_CONTRACT_ABA_RING
		? rca195_contract_freed_at : RCA195_CONTRACT_ABA_RING;
	for (i = 0; i < span; i++)
	{
		if (rca195_contract_freed[i] == p)
		{
			RCA195_CONTRACT_INC(rca195_contract_aba);
			RCA195ContractLog("ABA_REUSE ptr=%p", p);
			return;
		}
	}
}

/* Per-call summary, written once after the public decode returns.
 *
 * Every counter below has a matching log line, so the runner compares the two.
 * If a line was lost, the summary disagrees with the tally and the call is
 * scored as an instrument failure instead of contributing a quietly wrong zero.
 * This is what makes "WFREE = 0" a measurement rather than an assumption. */
static void RCA195ContractResult(int sdk_error, int result_false)
{
	RCA195ContractInit();
	RCA195ContractLog("RESULT sdk_error=%d result_false=%d claims=%llu "
					  "executed=%llu stale=%llu drain_short=%llu skipped=%llu "
					  "walloc=%llu wfree=%llu aba=%llu precedence=%llu "
					  "realloc_enter=%llu realloc_early=%llu",
					  sdk_error, result_false,
					  rca195_contract_claims, rca195_contract_executed,
					  rca195_contract_stale, rca195_contract_drain_short,
					  rca195_contract_skip, rca195_contract_walloc,
					  rca195_contract_wfree, rca195_contract_aba,
					  rca195_contract_precedence,
					  rca195_contract_realloc_calls,
					  rca195_contract_realloc_early);
}

#define RCA195_CONTRACT_KEEP_ERROR(field, value)                              \
	do {                                                                      \
		int rca195_v_ = (int)(value);                                         \
		if (rca195_v_ != 0) {                                                 \
			if ((int)(field) == 0) {                                          \
				(field) = (value);                                            \
			} else if ((int)(field) != rca195_v_) {                           \
				RCA195ContractPrecedence((int)(field), rca195_v_);            \
			}                                                                 \
		}                                                                     \
	} while (0)

#endif /* HYPERCINE_RCA195_CONTRACT_H */
