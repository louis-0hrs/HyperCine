#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Re-derive the RCA-160 all-channel claim from the frozen upstream tree.

The 2026-10-28 report on #160 claims a specific readiness defect: the released
``BANDS_ALL_VALID`` predicate accepts ``valid == started`` and therefore reports
"all bands valid" for a wavelet whose band 0 was never published, and the
spatial consumer of the adjacent producer reads ``wavelet[1]->band[0]`` under
that predicate. This module re-reads that claim out of ``upstream/`` instead of
trusting the prose.

Two kinds of statement are kept apart, as they must be:

* parsed now from the tree -- macro form, guard placement, call order, lock
  coverage, preprocessor reachability;
* enumerated -- the flag-pair space is small enough to be exhausted instead of
  argued about.

Every anchor is matched structurally (whitespace-normalised, comment-stripped)
so a rebase that moves lines does not silently pass, and the resolved line
numbers are printed for the reader. A contract failure exits 1; it never
reports a pass for a tree it could not parse.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[1]
# The self-test replays every claim against a scratch copy, so the tree root has
# to be overridable. An override is refused unless it actually looks like a
# checkout, otherwise a typo would produce "no anchors found" instead of a
# clear failure.
ROOT = Path(os.environ.get("RCA160_ROOT", REPO)).resolve()
# The scratch tree used by the self-test is a bare "upstream" copy, while the
# repository root keeps upstream/ as a subdirectory. Both layouts are accepted;
# anything else is refused so a typo cannot read as "no anchors found".
UPSTREAM = ROOT / "upstream" if (ROOT / "upstream/Codec/image.h").exists() else ROOT
if not (UPSTREAM / "Codec/image.h").exists():
    raise SystemExit("RCA160_ROOT=%s contains no Codec/image.h" % ROOT)

IMAGE_H = UPSTREAM / "Codec/image.h"
DECODER_C = UPSTREAM / "Codec/decoder.c"
ENTROPY_C = UPSTREAM / "Codec/entropy_threading.c"
WAVELET_C = UPSTREAM / "Codec/wavelet.c"
SPATIAL_C = UPSTREAM / "Codec/spatial.c"
CONFIG_H = UPSTREAM / "Codec/config.h"

BUILD_FILES = ("CMakeLists.txt", "upstream/CMakeLists.txt", "tests/CMakeLists.txt",
               "cmake")

# The bands of a spatial wavelet are 0..3 (one lowpass, three highpass), so the
# flag words carry four meaningful bits and the pair space is 256.
NUM_BANDS = 4
FLAG_BITS = 1 << NUM_BANDS


class ContractError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def read_text(path):
    """Single read helper: CRLF is normalised so no anchor is line-ending sensitive."""
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def lines_of(path):
    return read_text(path).split("\n")


def norm(text):
    """Collapse whitespace so tabs-versus-spaces cannot hide a moved statement."""
    return re.sub(r"\s+", " ", text).strip()


def strip_comment(line):
    """Remove a trailing // comment. Block comments are handled by the callers
    that need them; every anchor here is single-line."""
    stripped = line.strip()
    if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
        return ""
    index = line.find("//")
    return line if index < 0 else line[:index]


DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")


def _normalise_condition(keyword, tail):
    """Return the condition text without a trailing comment."""
    text = tail.split("//", 1)[0].strip()
    if keyword in ("ifdef", "ifndef"):
        return "defined %s" % text
    return re.sub(r"\s+", "", text)


def conditional_stack(path, line_number):
    """Return the enclosing preprocessor frames for a 1-based line.

    Each frame is ``(keyword, condition, branch)`` where ``branch`` is ``then``
    or ``else``. Reachability is resolved from nesting, not from the nearest
    ``#if`` above: several ``#if DEBUG`` blocks sit within a dozen lines of each
    other in this file, and a proximity search keeps answering for a guard that
    was removed.
    """
    stack = []
    for number, raw in enumerate(lines_of(path), start=1):
        match = DIRECTIVE.match(raw)
        if match:
            keyword, tail = match.group(1), match.group(2)
            if keyword in ("if", "ifdef", "ifndef"):
                stack.append([keyword, _normalise_condition(keyword, tail), "then"])
            elif keyword == "elif":
                if not stack:
                    raise ContractError("%s:%d #elif without #if" % (path.name, number))
                stack[-1] = [keyword, _normalise_condition("if", tail), "then"]
            elif keyword == "else":
                if not stack:
                    raise ContractError("%s:%d #else without #if" % (path.name, number))
                stack[-1][2] = "else"
            elif keyword == "endif":
                if not stack:
                    raise ContractError("%s:%d unmatched #endif" % (path.name, number))
                stack.pop()
        if number == line_number:
            return [tuple(frame) for frame in stack]
    raise ContractError("%s has no line %d" % (path.name, line_number))


def config_integers(path=CONFIG_H):
    """Read the numeric #define values this tree fixes.

    Only literal integers are collected: an expression such as
    ``(_THREADED_DECODER)`` is recorded as unreadable rather than guessed, so a
    claim cannot rest on an evaluation this module did not perform.
    """
    values = {}
    pattern = re.compile(r"^\s*#define\s+([A-Za-z_]\w*)\s+(\S+)\s*$")
    for raw in lines_of(path):
        match = pattern.match(raw.split("//", 1)[0].rstrip())
        if not match:
            continue
        name, value = match.group(1), match.group(2)
        if re.fullmatch(r"-?\d+", value):
            values[name] = int(value)
    return values


def evaluate_condition(condition, table):
    """Return True, False, or None when a simple integer condition folds.

    Handles ``NAME``, ``NAME == N`` and ``NAME != N`` over constants read from
    config.h. Anything else returns None: unknown is never silently true.
    """
    text = condition.strip()
    if text in ("0", "1"):
        return text == "1"
    match = re.fullmatch(r"([A-Za-z_]\w*)\s*(==|!=)\s*(-?\d+)", text)
    if match and match.group(1) in table:
        left = table[match.group(1)]
        right = int(match.group(3))
        return (left == right) if match.group(2) == "==" else (left != right)
    if re.fullmatch(r"[A-Za-z_]\w*", text) and text in table:
        return table[text] != 0
    return None


def is_dead(path, line_number, table=None):
    """True when no build of this tree reaches the line.

    A line is dead when an enclosing branch was cut by a conditional that folds
    to a constant: the ``then`` of a false condition or the ``else`` of a true
    one. Conditions are evaluated against the literal integers this tree's
    config.h defines, so ``#if _DELAYED_THREAD_START==0`` is resolved rather
    than quoted as an open question.
    """
    if table is None:
        table = config_integers()
    for keyword, condition, branch in conditional_stack(path, line_number):
        value = evaluate_condition(condition, table)
        if value is None:
            continue
        if branch == "then" and value is False:
            return True
        if branch == "else" and value is True:
            return True
    return False


def conditional_context(path, line_number):
    """Return the enclosing conditionals rendered as short strings."""
    rendered = []
    for keyword, condition, branch in conditional_stack(path, line_number):
        rendered.append("%s %s%s" % (keyword, condition, " <else>" if branch == "else" else ""))
    return rendered


def find_unique(path, anchor, expected=1, comment_free=True):
    """Resolve a normalised anchor to line numbers, failing closed on drift."""
    wanted = norm(anchor)
    hits = []
    for number, raw in enumerate(lines_of(path), start=1):
        candidate = strip_comment(raw) if comment_free else raw
        if norm(candidate) == wanted:
            hits.append(number)
    require(len(hits) == expected,
            "%s: anchor %r matched %d times, expected %d"
            % (path.name, wanted[:60], len(hits), expected))
    return hits


def find_live(path, anchor, expected=1):
    """Resolve an anchor only at sites that are compiled in this configuration.

    wavelet.c carries two dead copies of the lowpass entry point, both inside
    ``#if 0`` blocks. Matching the signature without resolving its conditional
    nesting picks all three, which is exactly the proximity mistake this file
    avoids elsewhere.
    """
    wanted = norm(anchor)
    hits = []
    for number, raw in enumerate(lines_of(path), start=1):
        if norm(strip_comment(raw)) == wanted and not is_dead(path, number):
            hits.append(number)
    require(len(hits) == expected,
            "%s: live anchor %r matched %d times, expected %d"
            % (path.name, wanted[:60], len(hits), expected))
    return hits


def enclosing_block(path, line_number, open_token="{", close_token="}"):
    """Return the (first, last) line of the innermost brace block holding a line.

    Walking outwards from the line rather than forwards from a guessed start
    keeps the answer correct when unrelated braces are added or removed above.
    """
    lines = lines_of(path)
    depth = 0
    start = None
    for number in range(line_number, 0, -1):
        raw = strip_comment(lines[number - 1])
        depth += raw.count(close_token) - raw.count(open_token)
        if depth < 0:
            start = number
            break
    require(start is not None,
            "%s:%d is not inside a %s block" % (path.name, line_number, open_token))

    depth = 0
    for number in range(start, len(lines) + 1):
        raw = strip_comment(lines[number - 1])
        depth += raw.count(open_token) - raw.count(close_token)
        if depth == 0 and number > start:
            return start, number
    raise ContractError("%s: unbalanced braces after line %d" % (path.name, start))


def function_region(path, start_line):
    """Return the (first, last) line of the block that opens at or after start_line.

    Brace counting on comment-stripped text: these files put opening braces on
    their own line often enough that a fixed text window would drift.
    """
    lines = lines_of(path)
    depth = 0
    opened = False
    for number in range(start_line, len(lines) + 1):
        raw = strip_comment(lines[number - 1])
        depth += raw.count("{") - raw.count("}")
        if "{" in raw:
            opened = True
        if opened and depth <= 0:
            return start_line, number
    raise ContractError("%s: unbalanced braces after line %d" % (path.name, start_line))


def find_all(path, needle, comment_free=True):
    wanted = norm(needle)
    hits = []
    for number, raw in enumerate(lines_of(path), start=1):
        candidate = strip_comment(raw) if comment_free else raw
        if wanted in norm(candidate):
            hits.append(number)
    return hits


def count_prefixed(path, prefix):
    """Count call sites of a helper, excluding its definition and any commented line."""
    wanted = norm(prefix)
    hits = []
    for number, raw in enumerate(lines_of(path), start=1):
        candidate = norm(strip_comment(raw))
        if wanted in candidate and not re.match(r"^(static\s+)?bool\s", candidate):
            hits.append(number)
    return hits


# --------------------------------------------------------------------------
# Claim 1 -- the released predicate is the equality form, not a full mask.
# --------------------------------------------------------------------------

def check_macro_form(report):
    lines = lines_of(IMAGE_H)
    released = [n for n, raw in enumerate(lines, start=1)
                if norm(raw).startswith("#define BANDS_ALL_VALID(wavelet)")]
    require(len(released) == 1,
            "image.h: expected exactly one live BANDS_ALL_VALID definition, found %d"
            % len(released))
    line = released[0]
    definition = normalize_expression(lines[line - 1].split(")", 1)[1])
    require(definition == "((wavelet)->band_valid_flags == (wavelet)->band_started_flags)",
            "image.h:%d: BANDS_ALL_VALID is not the released equality form: %s"
            % (line, definition))

    # The strict mask form must exist only as a comment, otherwise the claim
    # "the correct predicate was replaced by an equality test" is stale.
    strict = [n for n, raw in enumerate(lines, start=1)
              if "band_valid_flags ==" in raw
              and "(1 << (wavelet)->num_bands) - 1" in raw
              and raw.strip().startswith("//")]
    require(len(strict) == 1,
            "image.h: expected the strict mask form to survive as exactly one "
            "commented line, found %d" % len(strict))

    report["macro"]["define_line"] = line
    report["macro"]["strict_form_commented_at"] = strict[0]
    report["macro"]["expression"] = definition


def normalize_expression(expression):
    text = re.sub(r"\s+", " ", expression).strip()
    return text


# --------------------------------------------------------------------------
# Claim 2 -- the flag-pair space is small: exhaust it.
# --------------------------------------------------------------------------

def enumerate_flag_pairs(report):
    """Evaluate released and strict predicates over every (valid, started) pair."""
    released_accept = []
    released_missing_lowpass = []
    strict_accept = []
    strict_missing_lowpass = []
    full_mask = FLAG_BITS - 1

    for valid in range(FLAG_BITS):
        for started in range(FLAG_BITS):
            if valid == started:                       # released predicate
                released_accept.append((valid, started))
                if not valid & 1:
                    released_missing_lowpass.append((valid, started))
            if valid == full_mask:                     # strict predicate
                strict_accept.append((valid, started))
                if not valid & 1:
                    strict_missing_lowpass.append((valid, started))

    require(len(released_accept) == FLAG_BITS,
            "flag space: released predicate accepted %d pairs, expected %d"
            % (len(released_accept), FLAG_BITS))
    require(len(released_missing_lowpass) == FLAG_BITS // 2,
            "flag space: released predicate accepted %d band0-absent pairs, expected %d"
            % (len(released_missing_lowpass), FLAG_BITS // 2))
    require(len(strict_missing_lowpass) == 0,
            "flag space: strict predicate must accept no band0-absent pair")
    require((0xE, 0xE) in released_missing_lowpass,
            "flag space: the reported 0xE/0xE pair must be accepted by the "
            "released predicate with band 0 absent")

    report["flag_space"] = {
        "pairs_examined": FLAG_BITS * FLAG_BITS,
        "released_accepted": len(released_accept),
        "released_accepted_without_lowpass": len(released_missing_lowpass),
        "released_without_lowpass_pairs": ["0x%X/0x%X" % pair for pair in released_missing_lowpass],
        "strict_accepted": len(strict_accept),
        "strict_accepted_without_lowpass": len(strict_missing_lowpass),
    }
    return released_missing_lowpass


# --------------------------------------------------------------------------
# Claim 3 -- admission and consumer both decide under the same predicate.
# --------------------------------------------------------------------------

def check_admission_sites(report):
    # Locate the queue-admission decision by its own invariant (`done == 0`),
    # then assert that it also tests BANDS_ALL_VALID. Checking the predicate
    # first would make a mutation of the predicate fail as "anchor missing"
    # instead of as "the admission no longer waits for readiness".
    decision = find_all(ENTROPY_C, "data->queue[curr_entry].done == 0")
    require(len(decision) == 1,
            "entropy_threading.c: expected one queue-admission decision, found %d at %s"
            % (len(decision), decision))
    decision_line = decision[0]
    text = strip_comment(lines_of(ENTROPY_C)[decision_line - 1])
    require("BANDS_ALL_VALID(wavelet)" in norm(text),
            "entropy_threading.c:%d no longer tests BANDS_ALL_VALID in the "
            "admission decision: %r" % (decision_line, norm(text)))

    # The decision must be inside a region a Lock opened and an Unlock closes.
    lock = [n for n in find_all(ENTROPY_C, "Lock(&decoder->entropy_worker_new.lock)")
            if n < decision_line and not is_dead(ENTROPY_C, n)]
    all_unlocks = [n for n in find_all(ENTROPY_C, "Unlock(&decoder->entropy_worker_new.lock)")
                   if not is_dead(ENTROPY_C, n)]
    unlock_before = [n for n in all_unlocks if n < decision_line]
    require(lock and (not unlock_before or lock[-1] > unlock_before[-1]),
            "entropy_threading.c: the admission decision at line %d is no longer "
            "under the entropy lock (last lock %s, last unlock before it %s)"
            % (decision_line, lock[-1] if lock else None,
               unlock_before[-1] if unlock_before else None))

    # The post-unlock re-check is the window the report names, so it has to
    # exist, use the same predicate, and sit after the unlocking line.
    closing = [n for n in all_unlocks if n > decision_line]
    require(closing,
            "entropy_threading.c: no unlock follows the admission decision")
    recheck_candidates = [n for n in find_all(ENTROPY_C, "if(wavelet && BANDS_ALL_VALID(wavelet))")
                          if n > closing[0]]
    require(len(recheck_candidates) == 1,
            "entropy_threading.c: expected one post-unlock re-check of "
            "BANDS_ALL_VALID(wavelet) after line %d, found %d"
            % (closing[0], len(recheck_candidates)))
    recheck = recheck_candidates[0]

    uses = find_all(ENTROPY_C, "BANDS_ALL_VALID(wavelet)")
    require(len(uses) == 3,
            "entropy_threading.c: expected 3 BANDS_ALL_VALID uses, found %d at %s"
            % (len(uses), uses))

    report["admission"] = {
        "uses": uses,
        "decision_line": decision_line,
        "decision_under_lock": True,
        "unlock_line": closing[0],
        "recheck_after_unlock_line": recheck,
    }


def check_consumer_guards(report):
    runtime = [n for n in find_all(DECODER_C, "if (!BANDS_ALL_VALID(wavelet))")
               if "(wavelet)" in strip_comment(lines_of(DECODER_C)[n - 1])]
    require(len(runtime) == 3,
            "decoder.c: expected 3 runtime BANDS_ALL_VALID guards, found %d at %s"
            % (len(runtime), runtime))

    temporal_assert = [n for n in find_all(DECODER_C, "assert(BANDS_ALL_VALID(temporal))")]
    require(len(temporal_assert) == 1,
            "decoder.c: expected exactly one temporal assert, found %d"
            % len(temporal_assert))

    commented = [n for n, raw in enumerate(lines_of(DECODER_C), start=1)
                 if raw.strip().startswith("//") and "assert(BANDS_ALL_VALID(wavelet))" in norm(raw)]
    require(len(commented) == 3,
            "decoder.c: expected the three spatial asserts to survive as comments, found %d"
            % len(commented))

    report["consumers"] = {
        "runtime_guards": runtime,
        "temporal_assert_only": temporal_assert[0],
        "commented_asserts": commented,
    }
    return runtime


# --------------------------------------------------------------------------
# Claim 4 -- in the spatial lowpass branch, publication follows the consumer read.
# --------------------------------------------------------------------------

def check_publication_order(report):
    # The invariant is a per-branch ordering, so it is tested per branch: after a
    # readiness guard, the inverse transform must come next and the publication
    # after it. A publish inserted above the call has to fail as "the publication
    # now precedes the transform" -- counting publications first would report it
    # as a count mismatch, which names the wrong cause.
    guards = [n for n, raw in enumerate(lines_of(DECODER_C), start=1)
              if norm(strip_comment(raw)) == "if (!BANDS_ALL_VALID(wavelet))"
              and "BANDS_ALL_VALID(wavelet)" in strip_comment(raw)]
    require(len(guards) == 3,
            "decoder.c: expected 3 runtime BANDS_ALL_VALID guards, found %d at %s"
            % (len(guards), guards))

    calls = [n for n, raw in enumerate(lines_of(DECODER_C), start=1)
             if "TransformInverseSpatialQuantLowpass(wavelet, lowpass, scratch, prescale, inverse_prescale);"
             in norm(strip_comment(raw))]
    publishes = [n for n, raw in enumerate(lines_of(DECODER_C), start=1)
                 if "UpdateWaveletBandValidFlags(decoder, lowpass, 0);"
                 in norm(strip_comment(raw))]
    require(len(calls) == 2,
            "decoder.c: expected 2 inverse transforms in the spatial lowpass "
            "branches but found %d at %s" % (len(calls), calls))

    for guard in guards[:2]:
        first_call = min((n for n in calls if n > guard), default=None)
        require(first_call is not None,
                "decoder.c: no inverse transform follows the readiness guard at %d"
                % guard)
        first_publish = min((n for n in publishes if n > guard), default=None)
        require(first_publish is None or first_call < first_publish,
                "decoder.c: the publication at %s now precedes the inverse "
                "transform at %d in the branch guarded at %d -- the "
                "read-before-write window is gone"
                % (first_publish, first_call, guard))

    runtime_calls, asserted_calls = assert_only_call_sites(DECODER_C, "AllLowpassBandsValid")
    require(not runtime_calls,
            "decoder.c: AllLowpassBandsValid gained a runtime call site at %s -- "
            "the assert-only finding is stale" % runtime_calls)
    require(asserted_calls,
            "decoder.c: AllLowpassBandsValid has no call site at all")

    report["publication"] = {
        "readiness_guards": guards,
        "inverse_transform_lines": calls,
        "publish_lines": publishes,
        "order": "guard -> inverse -> publish (per branch)",
    }


# --------------------------------------------------------------------------
# Claim 5 -- the consumed input really is band 0 of the parent wavelet.
# --------------------------------------------------------------------------

def check_read_reaches_band0(report):
    # Count the dispatches structurally: which `input->band[N]` is passed into
    # the descale kernel. Asserting `expected=2` on a band-specific anchor makes
    # "the consumer switched to band 1" fail as "anchor missing" instead of as
    # "the read no longer reaches band 0" -- which is the claim being tested.
    dispatches = []
    for number, raw in enumerate(lines_of(WAVELET_C), start=1):
        candidate = norm(strip_comment(raw))
        match = re.search(r"InvertSpatialQuantDescale16s\(\(PIXEL \*\)input->band\[(\d)\]", candidate)
        if match:
            dispatches.append((number, int(match.group(1))))
    require(dispatches,
            "wavelet.c: no InvertSpatialQuantDescale16s dispatch on input->band[N] found")
    require(len(dispatches) >= 2,
            "wavelet.c: expected the lowpass and highpass descale dispatches, found %d"
            % len(dispatches))
    bands = sorted({band for _, band in dispatches})
    require(bands == [0],
            "wavelet.c: the descale dispatch no longer reads band 0 (bands used: %s)"
            % bands)

    # The first dispatch belongs to the lowpass entry point reached at scale > 1.
    # Two dead copies of that entry point sit inside ``#if 0`` blocks, so the
    # definition is resolved by reachability, not by text.
    lowpass_fn = find_live(WAVELET_C,
                           "void TransformInverseSpatialQuantLowpass(IMAGE *input, IMAGE *output,")
    require(lowpass_fn[0] < dispatches[0][0],
            "wavelet.c: the lowpass entry point no longer precedes the descale dispatch")

    kernel = find_live(SPATIAL_C,
                       "void InvertSpatialQuantDescale16s(PIXEL *lowlow_band, int lowlow_pitch,")
    kernel_end = function_region(SPATIAL_C, kernel[0])[1]

    reads = [n for n in find_all(SPATIAL_C, "even += 11 * lowlow[column + 0 * lowlow_pitch];")
             if kernel[0] < n < kernel_end]
    require(reads,
            "spatial.c: the lowlow read no longer exists inside "
            "InvertSpatialQuantDescale16s (lines %d-%d)" % (kernel[0], kernel_end))

    report["band0_read"] = {
        "wavelet_dispatch_lines": [number for number, _ in dispatches],
        "wavelet_dispatch_bands": bands,
        "lowpass_entry_line": lowpass_fn[0],
        "spatial_kernel_line": kernel[0],
        "first_lowlow_read_lines": reads[:6],
    }


# --------------------------------------------------------------------------
# Claim 6 -- which flag word is written under which lock.
# --------------------------------------------------------------------------

def check_flag_writers(report):
    valid_fn = find_unique(DECODER_C, "void UpdateWaveletBandValidFlags(DECODER *decoder, IMAGE *wavelet, int band)")
    started_fn = find_unique(DECODER_C, "void UpdateWaveletBandStartedFlags(DECODER *decoder, IMAGE *wavelet, int band)")

    valid_range = function_region(DECODER_C, valid_fn[0])
    started_range = function_region(DECODER_C, started_fn[0])

    valid_locks = [n for n in find_all(DECODER_C, "Lock(&decoder->entropy_worker_new.lock)")
                   if valid_range[0] < n < valid_range[1] and not is_dead(DECODER_C, n)]
    started_locks = [n for n in find_all(DECODER_C, "Lock(&decoder->entropy_worker_new.lock)")
                     if started_range[0] < n < started_range[1] and not is_dead(DECODER_C, n)]
    require(len(valid_locks) == 1,
            "decoder.c: UpdateWaveletBandValidFlags no longer takes exactly one lock")
    require(len(started_locks) == 0,
            "decoder.c: UpdateWaveletBandStartedFlags now takes a lock -- the "
            "unprotected-writer finding is stale")

    # The started-flag writer is lock-free only because of this configuration
    # constant; resolve it instead of quoting the comment.
    config = read_text(CONFIG_H)
    match = re.search(r"^#define _DELAYED_THREAD_START\s+(\S+)", config, re.MULTILINE)
    require(match is not None, "config.h: _DELAYED_THREAD_START is no longer defined")
    require(match.group(1) == "1",
            "config.h: _DELAYED_THREAD_START is no longer 1, so the started-flag "
            "writer would take the lock")

    both = [n for n in find_all(DECODER_C, "wavelet->band_valid_flags |= BAND_VALID_MASK(band);")]
    require(len(both) == 1,
            "decoder.c: the valid-flag store is no longer a single site")

    report["writers"] = {
        "valid_flags_writer_line": valid_fn[0],
        "valid_flags_locks": valid_locks,
        "started_flags_writer_line": started_fn[0],
        "started_flags_locks": started_locks,
        "delayed_thread_start": match.group(1),
        "finding": "band_valid_flags is stored under the entropy lock; "
                   "band_started_flags is stored with no lock at all",
    }


# --------------------------------------------------------------------------
# Claim 7 -- the two "correct" guards are hollow and assert-only.
# --------------------------------------------------------------------------

def check_guard_classification(report):
    lines = lines_of(DECODER_C)

    definition = [n for n, raw in enumerate(lines, start=1)
                  if norm(raw).startswith("static bool AllTransformBandsValid(")]
    require(len(definition) == 1, "decoder.c: AllTransformBandsValid definition not found")
    context = conditional_context(DECODER_C, definition[0])
    require(context == ["if DEBUG||1"],
            "decoder.c: AllTransformBandsValid is no longer wrapped in '#if DEBUG || 1': %r"
            % context)

    # Its real test must still be commented out and replaced by a pointer test.
    body_start = definition[0]
    body_end = body_start + 60
    body = "\n".join(lines[body_start - 1:body_end])
    require("//if (!AllBandsValid(wavelet))" in body,
            "decoder.c: AllTransformBandsValid no longer has its real band test "
            "commented out")
    require("if(wavelet == NULL)" in body,
            "decoder.c: AllTransformBandsValid no longer falls back to a pointer test")

    lowpass_assert = find_unique(DECODER_C, "assert(AllLowpassBandsValid(transform_array, num_channels, frame_index));")
    runtime_calls, asserted_calls = assert_only_call_sites(DECODER_C, "AllLowpassBandsValid")
    require(asserted_calls == lowpass_assert,
            "decoder.c: the only AllLowpassBandsValid call sites are %s, but the "
            "assert resolves at %s" % (asserted_calls, lowpass_assert))
    assert_context = conditional_context(DECODER_C, lowpass_assert[0])
    require(assert_context == ["if DEBUG"],
            "decoder.c: the AllLowpassBandsValid assert is no longer '#if DEBUG'-only: %r"
            % assert_context)

    require(not runtime_calls,
            "decoder.c: AllLowpassBandsValid gained a runtime call site at %s -- "
            "the assert-only finding is stale" % runtime_calls)

    debug_macro = find_unique(DECODER_C, "#define DEBUG  (1 && _DEBUG)")
    require(debug_macro[0] < definition[0],
            "decoder.c: the DEBUG macro no longer precedes the helpers")

    defined_in_build = scan_build_files_for("_DEBUG")
    require(defined_in_build == [],
            "build files now define _DEBUG (%s) -- the 'never active in this tree' "
            "claim is stale" % defined_in_build)

    report["guards"] = {
        "AllTransformBandsValid": {
            "line": definition[0],
            "context": "if DEBUG || 1",
            "status": "hollow",
            "reason": "always compiled, real band test commented out, "
                      "pointer test only",
        },
        "AllLowpassBandsValid": {
            "assert_line": lowpass_assert[0],
            "context": "if DEBUG",
            "status": "assert-only",
            "reason": "DEBUG expands to (1 && _DEBUG); no build file in this tree "
                      "defines _DEBUG, so the call is absent from every "
                      "configuration this tree configures (a MSVC Debug "
                      "configuration would supply it through platform defaults)",
        },
        "_DEBUG_defined_in_build_files": defined_in_build,
    }


def call_sites(path, name):
    """Return call sites of a helper, excluding its definition and every
    commented-out line.

    Matching on a line number would break the moment an unrelated line is
    inserted above -- and it did, during the first self-test run.
    """
    hits = []
    pattern = re.compile(r"(^|[^A-Za-z0-9_])%s\s*\(" % re.escape(name))
    definition = re.compile(r"^(static\s+)?bool\s+%s\s*\(" % re.escape(name))
    for number, raw in enumerate(lines_of(path), start=1):
        candidate = norm(strip_comment(raw))
        if not candidate:
            continue
        if definition.match(candidate):
            continue
        if pattern.search(candidate) and not candidate.startswith("bool "):
            hits.append((number, candidate))
    return hits


def assert_only_call_sites(path, name):
    """Split a helper's call sites into assert-wrapped and runtime uses.

    "assert-only" is a claim about every call site, so both halves have to be
    counted: a single new runtime caller makes the classification false even
    though the assert remains.
    """
    runtime, asserted = [], []
    for number, text in call_sites(path, name):
        if re.search(r"\bassert\s*\(", text):
            asserted.append(number)
        else:
            runtime.append(number)
    return runtime, asserted


def scan_build_files_for(needle):
    """Search the build configuration for a macro definition, ignoring sources."""
    hits = []
    candidates = []
    for name in BUILD_FILES:
        path = ROOT / name
        if path.is_dir():
            candidates.extend(sorted(path.rglob("*.cmake")))
        elif path.exists():
            candidates.append(path)
    for path in candidates:
        text = read_text(path)
        for number, raw in enumerate(text.split("\n"), start=1):
            if needle in raw and "D" + needle in raw:
                hits.append("%s:%d" % (path.name, number))
    return hits


# --------------------------------------------------------------------------
# Claim 8 -- the remaining branches are not covered by the same dependency.
# --------------------------------------------------------------------------

def check_ungated_branches(report):
    lines = lines_of(DECODER_C)
    highpass = find_unique(DECODER_C, "else if (index == 3)")
    temporal = find_unique(DECODER_C, "else if (index == 2)")

    # index == 3 still decides with the same vacuous predicate on the child
    # wavelet, and its publication targets the highpass band.
    highpass_publish = find_unique(DECODER_C, "UpdateWaveletBandValidFlags(decoder, highpass, 1);")
    require(highpass_publish[0] > highpass[0],
            "decoder.c: the index==3 publication moved out of its branch")

    frame_publishes = find_all(DECODER_C, "UpdateWaveletBandValidFlags(decoder, frame[")
    require(len(frame_publishes) == 2,
            "decoder.c: expected the temporal branch to publish both frame "
            "wavelets, found %d" % len(frame_publishes))

    # The temporal branch must still guard only with an assert.
    temporal_window = lines[temporal[0] - 1:temporal[0] + 120]
    requires_band0 = [n for n, raw in enumerate(temporal_window, start=temporal[0])
                      if "AllLowpassBandsValid" in strip_comment(raw)
                      or "BAND_VALID_MASK(0)" in strip_comment(raw)
                      and "assert" not in strip_comment(raw)]
    require(requires_band0 == [],
            "decoder.c: the temporal branch gained a runtime band-0 requirement "
            "at %s -- the ungated-branch finding is stale" % requires_band0)

    report["ungated"] = {
        "highpass_branch_line": highpass[0],
        "temporal_branch_line": temporal[0],
        "temporal_runtime_band0_check": False,
        "frame_publish_lines": frame_publishes,
        "scope_note": "the frame wavelets that HALF and FULL output read receive "
                      "band 0 from the temporal transform; that transform is "
                      "guarded only by an assert",
    }


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def validate():
    report = {"macro": {}, "consumers": {}}
    for check in (check_macro_form, enumerate_flag_pairs, check_admission_sites,
                  check_consumer_guards, check_publication_order,
                  check_read_reaches_band0, check_flag_writers,
                  check_guard_classification, check_ungated_branches):
        check(report)

    report["upstream_tree"] = upstream_status()
    report["verdict"] = "READINESS_CONTRACT_PARSED"
    return report


def upstream_status():
    import subprocess
    try:
        result = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain", "upstream/"],
                                capture_output=True, text=True, timeout=30)
        return "clean" if not result.stdout.strip() else "dirty"
    except (OSError, subprocess.SubprocessError):
        return "unknown"

def print_report(report):
    macro = report["macro"]
    space = report["flag_space"]
    print("BANDS_ALL_VALID released at image.h:%d" % macro["define_line"])
    print("  expression: %s" % macro["expression"])
    print("  strict mask form survives only as a comment at image.h:%d"
          % macro["strict_form_commented_at"])
    print()
    print("flag-pair space: %d pairs examined" % space["pairs_examined"])
    print("  released predicate accepts %d; %d of them have band 0 absent"
          % (space["released_accepted"], space["released_accepted_without_lowpass"]))
    print("  band0-absent accepted pairs: %s" % ", ".join(space["released_without_lowpass_pairs"]))
    print("  strict predicate accepts %d; %d with band 0 absent"
          % (space["strict_accepted"], space["strict_accepted_without_lowpass"]))
    print()
    admission = report["admission"]
    print("admission: uses at %s, decision at line %d under the entropy lock, "
          "re-check after unlock at line %d"
          % (admission["uses"], admission["decision_line"], admission["recheck_after_unlock_line"]))
    consumers = report["consumers"]
    print("consumers: runtime guards %s, temporal assert-only at %d"
          % (consumers["runtime_guards"], consumers["temporal_assert_only"]))
    publication = report["publication"]
    print("publication order: %s (inverse %s -> publish %s)"
          % (publication["order"], publication["inverse_transform_lines"],
             publication["publish_lines"]))
    reads = report["band0_read"]
    print("band0 read: dispatch %s in %s() -> kernel at spatial.c:%d"
          % (reads["wavelet_dispatch_lines"], "TransformInverseSpatialQuantLowpass",
             reads["spatial_kernel_line"]))
    writers = report["writers"]
    print("writers: valid-flags at decoder.c:%d with lock(s) %s; started-flags at "
          "decoder.c:%d with lock(s) %s (_DELAYED_THREAD_START=%s)"
          % (writers["valid_flags_writer_line"], writers["valid_flags_locks"],
             writers["started_flags_writer_line"], writers["started_flags_locks"],
             writers["delayed_thread_start"]))
    print("  %s" % writers["finding"])
    guards = report["guards"]
    print("guards: AllTransformBandsValid:%d [%s] %s"
          % (guards["AllTransformBandsValid"]["line"],
             guards["AllTransformBandsValid"]["context"],
             guards["AllTransformBandsValid"]["status"]))
    print("        AllLowpassBandsValid assert:%d [%s] %s"
          % (guards["AllLowpassBandsValid"]["assert_line"],
             guards["AllLowpassBandsValid"]["context"],
             guards["AllLowpassBandsValid"]["status"]))
    print("        _DEBUG defined by build files: %s"
          % (guards["_DEBUG_defined_in_build_files"] or "no"))
    ungated = report["ungated"]
    print("ungated: index==3 at %d, temporal at %d with no runtime band-0 check"
          % (ungated["highpass_branch_line"], ungated["temporal_branch_line"]))
    print()
    print("upstream/: %s" % report["upstream_tree"])
    print("VERDICT: %s" % report["verdict"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="emit the resolved contract as JSON")
    parser.add_argument("--self-test", action="store_true",
                        help="mutate a scratch copy and require every claim to be rejected")
    args = parser.parse_args(argv)

    if args.self_test:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "rca160_self_test", REPO / "tools/rca160_all_channel_selftest.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.run()

    try:
        report = validate()
    except ContractError as error:
        print("RCA160-CONTRACT: FAIL -- %s" % error, file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_report(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
