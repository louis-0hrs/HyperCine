// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Test-only RCA-160 band-0 admission gate and read-only probes. This header is
// never part of the frozen upstream tree; it is applied to a private SDK copy
// outside the repository. Transform math, the frame writer and scheduling are
// unchanged. The gate is enabled only by RCA160_STRICT_GATE and is a
// diagnostic, not a portable synchronization contract.
#ifndef HYPERCINE_RCA160_PROBE_H
#define HYPERCINE_RCA160_PROBE_H

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
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

#define RCA160_MAX_SPINS 50000000ULL
#define RCA160_MAX_WAIT_MS 5000ULL

static FILE *rca160_log_file = NULL;
static int rca160_log_ready = 0;
static int rca160_strict = 0;

#ifdef _WIN32
static CRITICAL_SECTION rca160_mutex;
static INIT_ONCE rca160_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER rca160_frequency;

static BOOL CALLBACK RCA160InitOnce(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	const char *path;
	const char *strict;
	(void)once;
	(void)parameter;
	(void)context;

	InitializeCriticalSection(&rca160_mutex);
	QueryPerformanceFrequency(&rca160_frequency);
	path = getenv("RCA160_PROBE_LOG");
	strict = getenv("RCA160_STRICT_GATE");
	rca160_strict = (strict != NULL && strict[0] != '\0' && strcmp(strict, "0") != 0) ? 1 : 0;
	if (path != NULL && path[0] != '\0')
	{
		if (fopen_s(&rca160_log_file, path, "a") == 0 && rca160_log_file != NULL)
			rca160_log_ready = 1;
	}
	return TRUE;
}

static void RCA160EnsureInit(void)
{
	InitOnceExecuteOnce(&rca160_once, RCA160InitOnce, NULL, NULL);
}

static unsigned long long RCA160NowNs(void)
{
	LARGE_INTEGER counter;
	RCA160EnsureInit();
	QueryPerformanceCounter(&counter);
	return (unsigned long long)((double)counter.QuadPart * 1000000000.0 /
								(double)rca160_frequency.QuadPart);
}

static unsigned long RCA160ThreadId(void)
{
	return (unsigned long)GetCurrentThreadId();
}

static void RCA160SpinYield(void)
{
	YieldProcessor();
}
#else
static pthread_mutex_t rca160_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t rca160_once = PTHREAD_ONCE_INIT;
static struct timespec rca160_epoch;

static void RCA160InitOnce(void)
{
	const char *path = getenv("RCA160_PROBE_LOG");
	const char *strict = getenv("RCA160_STRICT_GATE");
	clock_gettime(CLOCK_MONOTONIC, &rca160_epoch);
	rca160_strict = (strict != NULL && strict[0] != '\0' && strcmp(strict, "0") != 0) ? 1 : 0;
	if (path != NULL && path[0] != '\0')
	{
		rca160_log_file = fopen(path, "a");
		if (rca160_log_file != NULL)
			rca160_log_ready = 1;
	}
}

static void RCA160EnsureInit(void)
{
	pthread_once(&rca160_once, RCA160InitOnce);
}

static unsigned long long RCA160NowNs(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)(now.tv_sec - rca160_epoch.tv_sec) * 1000000000ULL +
		   (unsigned long long)(now.tv_nsec - rca160_epoch.tv_nsec);
}

static unsigned long RCA160ThreadId(void)
{
	return (unsigned long)pthread_self();
}

static void RCA160SpinYield(void)
{
	sched_yield();
}
#endif

static void RCA160Log(const char *format, ...)
{
	va_list arguments;
	RCA160EnsureInit();
	if (!rca160_log_ready)
		return;
#ifdef _WIN32
	EnterCriticalSection(&rca160_mutex);
#else
	pthread_mutex_lock(&rca160_mutex);
#endif
	va_start(arguments, format);
	vfprintf(rca160_log_file, format, arguments);
	va_end(arguments);
	fputc('\n', rca160_log_file);
	fflush(rca160_log_file);
#ifdef _WIN32
	LeaveCriticalSection(&rca160_mutex);
#else
	pthread_mutex_unlock(&rca160_mutex);
#endif
}

// Replaces BANDS_ALL_VALID(wavelet) at the two spatial inverse call sites.
// Gate off executes the original predicate only. Gate on additionally requires
// band 0 of the input wavelet to be valid while retaining ready high bands.
static int RCA160Band0Gate(DECODER *decoder, const char *site, int channel,
						   int index, IMAGE *wavelet)
{
	unsigned long long start_ns;
	RCA160EnsureInit();
	start_ns = RCA160NowNs();
	const unsigned long tid = RCA160ThreadId();
	const int strict = rca160_strict;
	uint32_t valid = (wavelet != NULL) ? wavelet->band_valid_flags : 0;
	uint32_t started = (wavelet != NULL) ? wavelet->band_started_flags : 0;
	const unsigned int num_bands = (wavelet != NULL) ? wavelet->num_bands : 0;
	unsigned long long spins = 0;
	unsigned long long elapsed_ms = 0;
	int admitted;

	RCA160Log("GATE_ENTRY site=%s ch=%d idx=%d strict=%d wptr=%p valid=%08x started=%08x num_bands=%u tid=%lu t_ns=%llu",
			  site, channel, index, strict, (void *)wavelet, valid, started, num_bands,
			  tid, start_ns);

	if (!strict)
	{
		admitted = (valid == started);
		RCA160Log("GATE_RESULT site=%s ch=%d idx=%d strict=0 wptr=%p admit=%d spins=0 elapsed_ms=0 final_valid=%08x final_started=%08x tid=%lu t_ns=%llu",
				  site, channel, index, (void *)wavelet, admitted, valid, started,
				  tid, RCA160NowNs());
		return admitted;
	}

	while (!((valid & 1u) != 0 && valid == started))
	{
		if (spins >= RCA160_MAX_SPINS)
		{
			elapsed_ms = (RCA160NowNs() - start_ns) / 1000000ULL;
			RCA160Log("GATE_TIMEOUT site=%s ch=%d idx=%d strict=1 wptr=%p spins=%llu elapsed_ms=%llu final_valid=%08x final_started=%08x reason=spin_limit tid=%lu t_ns=%llu",
					  site, channel, index, (void *)wavelet, spins, elapsed_ms, valid, started,
					  tid, RCA160NowNs());
			return 0;
		}
		++spins;
		RCA160SpinYield();
		if ((spins & 0x3ffu) == 0)
		{
			elapsed_ms = (RCA160NowNs() - start_ns) / 1000000ULL;
			if (elapsed_ms >= RCA160_MAX_WAIT_MS)
			{
				RCA160Log("GATE_TIMEOUT site=%s ch=%d idx=%d strict=1 wptr=%p spins=%llu elapsed_ms=%llu final_valid=%08x final_started=%08x reason=time_limit tid=%lu t_ns=%llu",
						  site, channel, index, (void *)wavelet, spins, elapsed_ms, valid, started,
						  tid, RCA160NowNs());
				return 0;
			}
		}
		valid = wavelet->band_valid_flags;
		started = wavelet->band_started_flags;
	}

	if (!(valid & 1u) || valid != started)
	{
		elapsed_ms = (RCA160NowNs() - start_ns) / 1000000ULL;
		RCA160Log("GATE_TIMEOUT site=%s ch=%d idx=%d strict=1 wptr=%p spins=%llu elapsed_ms=%llu final_valid=%08x final_started=%08x reason=predicate tid=%lu t_ns=%llu",
				  site, channel, index, (void *)wavelet, spins, elapsed_ms, valid, started,
				  tid, RCA160NowNs());
		return 0;
	}

	elapsed_ms = (RCA160NowNs() - start_ns) / 1000000ULL;
	RCA160Log("GATE_ADMIT site=%s ch=%d idx=%d strict=1 wptr=%p spins=%llu elapsed_ms=%llu final_valid=%08x final_started=%08x tid=%lu t_ns=%llu",
			  site, channel, index, (void *)wavelet, spins, elapsed_ms, valid, started,
			  tid, RCA160NowNs());
	(void)decoder;
	return 1;
}

static void RCA160ProbeStarted(IMAGE *wavelet, int band, uint32_t old_started,
							   uint32_t new_started, unsigned long long t_ns,
							   unsigned long tid)
{
	RCA160Log("BAND_STARTED wptr=%p band=%d old_started=%08x new_started=%08x tid=%lu t_ns=%llu",
			  (void *)wavelet, band, old_started, new_started, tid, t_ns);
}

static void RCA160ProbePublish(IMAGE *wavelet, int band, uint32_t old_valid,
							   uint32_t old_started, uint32_t new_valid,
							   uint32_t new_started, unsigned long long t_ns,
							   unsigned long tid)
{
	RCA160Log("BAND_PUBLISH wptr=%p band=%d old_valid=%08x old_started=%08x new_valid=%08x new_started=%08x tid=%lu t_ns=%llu",
			  (void *)wavelet, band, old_valid, old_started, new_valid, new_started,
			  tid, t_ns);
}

static void RCA160ProbeRoute(const char *site, int width, int height, int32_t pitch,
							 int format, int encode_format, int num_channels,
							 int chroma_offset, int precision)
{
	RCA160Log("ROUTE site=%s width=%d height=%d pitch=%d format=%d encode_format=%d channels=%d chroma_offset=%d precision=%d tid=%lu t_ns=%llu",
			  site, width, height, pitch, format, encode_format, num_channels,
			  chroma_offset, precision, RCA160ThreadId(), RCA160NowNs());
}

#endif
