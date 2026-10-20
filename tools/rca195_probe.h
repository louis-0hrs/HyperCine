// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// RCA-195 Milestone-1 completion probe. TEST-ONLY, never part of the frozen
// upstream tree: it is applied to a private out-of-repo SDK copy by
// tools/apply_rca195_probe.py and built there.
//
// Two jobs, both read-only with respect to the decoder's arithmetic:
//
//  1. Record the transform-queue state *immediately before* the queue is reset
//     by WaitForTransformThread, so "was every queued transform claimed and
//     executed?" becomes measurable instead of inferred.
//  2. Optionally consume one admitted queue entry without executing its
//     reconstruction, so a claimed-but-unexecuted transform can be induced
//     deterministically rather than waited for.
//
// The skip switch fabricates a *failure input*. It is not a fix, not a
// candidate, and must never be enabled outside this probe.
//
// An earlier revision also declared a `RCA195_WITHHOLD` producer-suppression
// hook. It had no call site in any translation unit, so it could never fire and
// no run ever exercised it; it is removed rather than left looking like a
// capability. The absence of a producer is covered by the skip arms instead.
#ifndef HYPERCINE_RCA195_PROBE_H
#define HYPERCINE_RCA195_PROBE_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#define RCA195_MAX_ENTRIES 64

/* This header is included by more than one translation unit. Mutable state must
 * therefore have a single definition, or each unit gets its own copy and the
 * counters silently disagree (a skip counted in entropy_threading.c would read
 * as zero when the queue reset is logged from decoder.c). Exactly one unit
 * defines RCA195_PROBE_OWNER before including this header. */
#ifdef RCA195_PROBE_OWNER
#define RCA195_STATE
#else
#define RCA195_STATE extern
#endif

RCA195_STATE FILE *rca195_log;
RCA195_STATE int rca195_ready;
RCA195_STATE int rca195_skip_ch;
RCA195_STATE int rca195_skip_idx;
RCA195_STATE unsigned long long rca195_skip_count;
RCA195_STATE unsigned long long rca195_claim_count;
RCA195_STATE unsigned long long rca195_exec_count;

static void RCA195Init(void)
{
	const char *path;
	const char *skip;

	if (rca195_ready)
		return;
	rca195_ready = 1;

	/* Shared state starts zeroed; set the "no target" sentinels here. */
	rca195_skip_ch = -1;
	rca195_skip_idx = -1;

	path = getenv("RCA195_PROBE_LOG");
	if (path != NULL && path[0] != '\0')
	{
		rca195_log = fopen(path, "a");
	}


	/* Format: <channel>:<index>. Consumes the entry but never executes it. */
	skip = getenv("RCA195_SKIP");
	if (skip != NULL && skip[0] != '\0')
	{
		int ch = -1, idx = -1;
		if (sscanf(skip, "%d:%d", &ch, &idx) == 2)
		{
			rca195_skip_ch = ch;
			rca195_skip_idx = idx;
		}
	}
}

static void RCA195Log(const char *fmt, ...)
{
	va_list args;

	RCA195Init();
	if (rca195_log == NULL)
		return;

	va_start(args, fmt);
	vfprintf(rca195_log, fmt, args);
	va_end(args);
	fputc('\n', rca195_log);
	fflush(rca195_log);
}

/* Returns 1 when the admitted queue entry for this (channel, index) must be
 * consumed without executing its reconstruction. This fabricates the
 * "claimed but not executed" input; it is failure injection, not a fix. */
static int RCA195ShouldSkip(int channel, int index)
{
	RCA195Init();
	if (rca195_skip_ch < 0)
		return 0;
	if (channel != rca195_skip_ch || index != rca195_skip_idx)
		return 0;
	rca195_skip_count++;
	return 1;
}

/* Counted in the worker translation unit itself, so claim and execute cannot
 * disagree through a header copy. A call with claims > executes lost a transform
 * that was taken out of the queue and never performed -- whether we induced it
 * or the decoder did it on its own. */
static void RCA195LogClaim(int channel, int index)
{
	RCA195Init();
	rca195_claim_count++;
	RCA195Log("CLAIM ch=%d idx=%d", channel, index);
}

static void RCA195LogExecute(int channel, int index)
{
	RCA195Init();
	rca195_exec_count++;
	RCA195Log("EXEC ch=%d idx=%d", channel, index);
}

/* Dump the queue immediately before the reset that discards it. `entries` is the
 * number the caller is about to throw away. `admitted` and `unadmitted` are
 * passed in by the caller rather than assumed: hard-coding them made two of the
 * reported fields constants instead of measurements. */
static void RCA195LogQueueReset(int num_entries, int free_entry, int next_entry,
								int admitted, int unadmitted)
{
	RCA195Log("QUEUE_RESET num_entries=%d free_entry=%d next_entry=%d "
			  "admitted=%d unadmitted=%d skipped=%llu claimed=%llu executed=%llu "
			  "unexecuted=%lld",
			  num_entries, free_entry, next_entry,
			  admitted, unadmitted, rca195_skip_count,
			  rca195_claim_count, rca195_exec_count,
			  (long long)rca195_claim_count - (long long)rca195_exec_count);
}

/* Logged once per decode, after the public entry point has returned. */
static void RCA195LogResult(int sdk_error, int result_false, const char *stage)
{
	RCA195Log("RESULT stage=%s sdk_error=%d result_false=%d skipped=%llu",
			  stage, sdk_error, result_false, rca195_skip_count);
}

#endif /* HYPERCINE_RCA195_PROBE_H */
