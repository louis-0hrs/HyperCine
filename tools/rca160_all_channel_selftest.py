#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Prove the RCA-160 contract checker can fail, one mutation per claim.

Each case copies exactly the files the checker reads into a scratch tree, applies
one mutation, and requires the checker to reject it **for the stated reason**.
A non-zero exit alone proves nothing: any unrelated breakage also exits 1, and a
checker that crashes on every input would pass a bare exit-code assertion.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

TOOL = Path(__file__).resolve().parent / "rca160_all_channel_contract.py"
REPO = TOOL.parents[1]

# Exactly the files the checker reads. Copying more lets a stale artefact mask a
# mutation; copying fewer turns a real check into a FileNotFoundError.
COPIED_SOURCES = (
    "Codec/image.h",
    "Codec/decoder.c",
    "Codec/entropy_threading.c",
    "Codec/wavelet.c",
    "Codec/spatial.c",
    "Codec/config.h",
)

CASES = []


def case(name, file, old, new, expected, occurrences=1):
    CASES.append({
        "name": name,
        "file": file,
        "old": old,
        "new": new,
        "expected": expected,
        "occurrences": occurrences,
    })


# 1. The predicate was fixed: the equality form became the strict mask form.
case("predicate fixed to strict mask form",
     "Codec/image.h",
     "#define BANDS_ALL_VALID(wavelet)\t((wavelet)->band_valid_flags == (wavelet)->band_started_flags)",
     "#define BANDS_ALL_VALID(wavelet)\t((wavelet)->band_valid_flags == (uint32_t)((1 << (wavelet)->num_bands) - 1))",
     "not the released equality form")

# 2. The band-0 consumer guard gained a lowpass-specific test (the "it was
#    already checked" mutation).
case("consumer guard gained a band-0 specific test",
     "Codec/decoder.c",
     "if (!BANDS_ALL_VALID(wavelet))",
     "if (!BANDS_ALL_VALID(wavelet) || !(lowpass->band_valid_flags & BAND_VALID_MASK(0)))",
     "expected 3 runtime BANDS_ALL_VALID guards")

# 3. Publication moved before the consumer read, i.e. the dependency now exists.
#    The two spatial sites are indented differently (one at four tabs, one at
#    three), so this case is applied per line rather than by one exact string --
#    otherwise a single-tab difference would silently mutate only one site.
CASES.append({
    "name": "publication moved ahead of the inverse transform",
    "file": "Codec/decoder.c",
    "insert_before_each": "TransformInverseSpatialQuantLowpass(wavelet, lowpass, scratch, prescale, inverse_prescale);",
    "insert_line": "UpdateWaveletBandValidFlags(decoder, lowpass, 0);",
    "expected": "now precedes the inverse transform",
    "occurrences": 2,
})

# 4. The unprotected writer was fixed: the started-flag store now takes the lock.
case("started-flag writer gained a lock",
     "Codec/decoder.c",
     "#if _DELAYED_THREAD_START==0\n\t\tif(decoder->entropy_worker_new.pool.thread_count)\n\t\t\tLock(&decoder->entropy_worker_new.lock);\n#endif\n\n\t\twavelet->band_started_flags |= BAND_VALID_MASK(band);",
     "#if 1\n\t\tif(decoder->entropy_worker_new.pool.thread_count)\n\t\t\tLock(&decoder->entropy_worker_new.lock);\n#endif\n\n\t\twavelet->band_started_flags |= BAND_VALID_MASK(band);",
     "UpdateWaveletBandStartedFlags now takes a lock")

# 5. The hollow helper was made honest: its real band test is back.
case("AllTransformBandsValid real test restored",
     "Codec/decoder.c",
     "\t\t//if (!AllBandsValid(wavelet))\n\t\tif(wavelet == NULL)",
     "\t\tif (!AllBandsValid(wavelet))\n\t\tif(wavelet == NULL)",
     "no longer has its real band test commented out")

# 6. The correct lowpass check left its #if DEBUG block.
case("AllLowpassBandsValid assert left its DEBUG block",
     "Codec/decoder.c",
     "#if DEBUG\n\t\t\tassert(AllLowpassBandsValid(transform_array, num_channels, frame_index));\n#endif",
     "assert(AllLowpassBandsValid(transform_array, num_channels, frame_index));",
     "'#if DEBUG'-only")

# 7. The build started defining _DEBUG, so "never active" is no longer true.
case("build defines _DEBUG after all",
     "CMakeLists.txt",
     "project(HyperCine",
     "add_compile_options(-D_DEBUG=1)\nproject(HyperCine",
     "build files now define _DEBUG")

# 8. The admission decision left the lock.
case("admission decision dropped the readiness predicate",
     "Codec/entropy_threading.c",
     "if(data->queue[curr_entry].done == 0 && BANDS_ALL_VALID(wavelet))",
     "if(data->queue[curr_entry].done == 0)",
     "no longer tests BANDS_ALL_VALID in the admission decision")

# 9. The post-unlock re-check disappeared, so the named window is gone.
case("post-unlock re-check removed",
     "Codec/entropy_threading.c",
     "if(wavelet && BANDS_ALL_VALID(wavelet))",
     "if(wavelet)",
     "expected one post-unlock re-check of BANDS_ALL_VALID")

# 10. The consumed input stopped being band 0.
case("spatial consumer switched to band 1",
     "Codec/wavelet.c",
     "InvertSpatialQuantDescale16s((PIXEL *)input->band[0], input->pitch,",
     "InvertSpatialQuantDescale16s((PIXEL *)input->band[1], input->pitch,",
     "the descale dispatch no longer reads band 0",
     occurrences=2)

# 11. The temporal branch gained a real readiness requirement.
case("temporal branch gained a runtime band-0 check",
     "Codec/decoder.c",
     "\t\t\tassert(BANDS_ALL_VALID(temporal));",
     "\t\t\tif (!(temporal->band_valid_flags & BAND_VALID_MASK(0))) { decoder->error = CODEC_ERROR_BAD_FRAME; return; }\n\t\t\tassert(BANDS_ALL_VALID(temporal));",
     "the temporal branch gained a runtime band-0 requirement")

# 12. The configuration constant flipped, which would enable the writer's lock.
case("_DELAYED_THREAD_START flipped to 0",
     "Codec/config.h",
     "#define _DELAYED_THREAD_START\t1",
     "#define _DELAYED_THREAD_START\t0",
     "now takes a lock")


def build_scratch(root):
    """Copy only what the checker reads, preserving the relative layout."""
    (root / "upstream").mkdir(parents=True)
    for relative in COPIED_SOURCES:
        destination = root / "upstream" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / "upstream" / relative, destination)
    # The build file is read for the _DEBUG claim; copy it too so that claim is
    # tested rather than skipped.
    shutil.copyfile(REPO / "CMakeLists.txt", root / "CMakeLists.txt")


def apply_mutation(root, entry, normalise):
    relative = entry["file"]
    path = root / "upstream" / relative
    if not path.exists():
        path = root / relative
    original = path.read_text(encoding="utf-8", errors="replace")
    text = normalise(original)

    if entry.get("insert_before_each"):
        anchor = normalise(entry["insert_before_each"])
        insert = normalise(entry["insert_line"])
        mutated_lines = []
        applied = 0
        for line in text.split("\n"):
            if anchor in line:
                indent = line[:len(line) - len(line.lstrip())]
                mutated_lines.append(indent + insert)
                applied += 1
            mutated_lines.append(line)
        if applied < entry["occurrences"]:
            raise RuntimeError("%s: insert anchor matched %d lines, expected at least %d"
                               % (entry["name"], applied, entry["occurrences"]))
        mutated = "\n".join(mutated_lines)
    else:
        old = normalise(entry["old"])
        new = normalise(entry["new"])
        found = text.count(old)
        if found < entry["occurrences"]:
            raise RuntimeError("%s: mutation anchor found %d times, expected at least %d"
                               % (entry["name"], found, entry["occurrences"]))
        mutated = text.replace(old, new, entry["occurrences"])

    if norm_eq(mutated, text):
        raise RuntimeError("%s: mutation did not change the file" % entry["name"])
    path.write_text(mutated, encoding="utf-8", newline="\n")
    return mutated, text


def norm_eq(left, right):
    return left.strip() == right.strip()


def run_tool(root):
    import os
    environment = dict(os.environ)
    environment["RCA160_ROOT"] = str(root)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run([sys.executable, str(TOOL), "--json"], capture_output=True,
                            text=True, env=environment, timeout=180)
    return result.returncode, result.stderr


def normalise(text):
    return text.replace("\r\n", "\n")


def run():
    failures = []
    root = Path(tempfile.mkdtemp(prefix="rca160-selftest-"))
    try:
        pristine = root / "pristine"
        pristine.mkdir()
        build_scratch(pristine)
        code, stderr = run_tool(pristine)
        if code != 0:
            failures.append("pristine copy failed rc=%d: %s" % (code, stderr.strip()[:400]))
        else:
            print("pristine            rc=0  (positive control accepted)")

        for entry in CASES:
            case_root = root / ("mutation-%02d" % (CASES.index(entry) + 1))
            case_root.mkdir()
            build_scratch(case_root)
            try:
                apply_mutation(case_root, entry, normalise)
            except RuntimeError as error:
                failures.append("mutation did not apply: %s" % error)
                print("MUTATION-NOT-APPLIED %s" % entry["name"])
                continue
            code, stderr = run_tool(case_root)
            reason_present = entry["expected"] in stderr
            if code == 0:
                failures.append("%s: checker still passed" % entry["name"])
                print("SURVIVED            %s" % entry["name"])
            elif not reason_present:
                failures.append("%s: rejected without saying %r (got %r)"
                                % (entry["name"], entry["expected"], stderr.strip()[:300]))
                print("WRONG-REASON        %s -> %s" % (entry["name"], stderr.strip()[:160]))
            else:
                print("killed   rc=%d      %s" % (code, entry["name"]))

        # Restoring the original content must return the checker to green: a
        # checker that fails forever would also "kill" every mutation.
        restore = root / "restored"
        restore.mkdir()
        build_scratch(restore)
        first = CASES[0]
        apply_mutation(restore, first, normalise)
        path = restore / "upstream" / first["file"]
        path.write_text(normalise((REPO / "upstream" / first["file"]).read_text(
            encoding="utf-8", errors="replace")), encoding="utf-8", newline="\n")
        code, stderr = run_tool(restore)
        if code != 0:
            failures.append("restored copy failed rc=%d: %s" % (code, stderr.strip()[:300]))
            print("RESTORE-FAILED")
        else:
            print("restored            rc=0  (mutation reverted, checker green again)")
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print()
    if failures:
        print("RCA160-SELFTEST: FAIL (%d)" % len(failures))
        for failure in failures:
            print("  - %s" % failure)
        return 1
    print("RCA160-SELFTEST: PASS (%d mutations killed, reason asserted, positive "
          "control and restore green)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(run())
