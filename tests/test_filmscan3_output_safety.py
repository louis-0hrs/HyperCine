#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Exercise output ownership through the real standalone encoder executable."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest


ENCODER = None


class OutputSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hypercine-output-safety-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.inputs = self.root / "inputs"
        self.inputs.mkdir()
        self.input = self.inputs / "frame.bgra"
        self.input.write_bytes(bytes((12, 34, 56, 255)) * (128 * 24))
        self.input_hash = self.digest(self.input)
        self.output = self.root / "output"

    @staticmethod
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def invoke(self, *args, **kwargs):
        return subprocess.run([str(ENCODER), "--geometry", "128x24", *map(str, args)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=30, **kwargs)

    def refused(self, result):
        self.assertNotEqual(result.returncode, 0, "unsafe request was accepted")
        self.assertEqual(self.digest(self.input), self.input_hash, "retained input changed")

    def no_run(self):
        self.assertFalse(self.output.exists(), "failed request published a run")
        self.assertFalse(list(self.root.glob(".hypercine-incomplete-*")),
                         "invalid preflight created staging output")

    def symlink(self, link, target):
        try:
            link.symlink_to(target, target_is_directory=True)
        except OSError as error:
            if os.name == "nt" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows directory symlink privilege is unavailable")
            raise

    def test_existing_trace_survives_invalid_first_input(self):
        self.output.mkdir()
        sentinel = self.output / "trace.json"
        sentinel.write_bytes(b"retained trace evidence")
        before = self.digest(sentinel)
        self.refused(self.invoke(self.output, self.inputs / "missing.bgra"))
        self.assertEqual(self.digest(sentinel), before)

    def test_existing_run_survives_valid_input(self):
        self.output.mkdir()
        sentinel = self.output / "trace.json"
        sentinel.write_bytes(b"retained trace evidence")
        before = self.digest(sentinel)
        self.refused(self.invoke(self.output, self.input))
        self.assertEqual(self.digest(sentinel), before)
        self.assertEqual(sorted(p.name for p in self.output.iterdir()), ["trace.json"])

    def test_existing_empty_directory_is_refused(self):
        self.output.mkdir()
        self.refused(self.invoke(self.output, self.input))
        self.assertEqual(list(self.output.iterdir()), [])

    def test_existing_output_file_is_preserved(self):
        self.output.write_bytes(b"retained ordinary file")
        before = self.digest(self.output)
        self.refused(self.invoke(self.output, self.input))
        self.assertEqual(self.digest(self.output), before)

    def test_existing_output_symlink_is_preserved(self):
        self.symlink(self.output, self.inputs)
        self.refused(self.invoke(self.output, self.input))
        self.assertTrue(self.output.is_symlink())
        self.assertFalse((self.inputs / "trace.json").exists())

    def test_dangling_output_symlink_is_preserved(self):
        self.symlink(self.output, self.root / "missing-target")
        self.refused(self.invoke(self.output, self.input))
        self.assertTrue(self.output.is_symlink())
        self.assertFalse((self.root / "missing-target").exists())

    def test_output_nested_in_input_root_is_refused(self):
        nested = self.inputs / "new-output"
        self.refused(self.invoke(nested, self.input))
        self.assertFalse(nested.exists())
        self.assertEqual(list(self.inputs.iterdir()), [self.input])

    def test_parent_alias_into_input_root_is_refused(self):
        alias = self.root / "input-alias"
        self.symlink(alias, self.inputs)
        self.refused(self.invoke(alias / "new-output", self.input))
        self.assertFalse((self.inputs / "new-output").exists())

    def test_missing_output_parent_is_refused(self):
        self.refused(self.invoke(self.root / "missing" / "output", self.input))
        self.assertFalse((self.root / "missing").exists())

    def test_invalid_later_input_creates_no_output(self):
        invalid = self.inputs / "short.bgra"
        invalid.write_bytes(b"incomplete frame")
        self.refused(self.invoke(self.output, self.input, invalid))
        self.no_run()

    def test_directory_input_is_refused_without_output(self):
        self.refused(self.invoke(self.output, self.inputs))
        self.no_run()

    def test_truncated_or_excess_metadata_creates_no_output(self):
        metadata = self.inputs / "metadata.raw"
        for size in (44, 46):
            metadata.write_bytes(bytes(size))
            self.refused(self.invoke("--metadata-binary", metadata, self.output, self.input))
            self.no_run()

    def test_metadata_parent_is_retained(self):
        metadata_root = self.root / "metadata"
        metadata_root.mkdir()
        metadata = metadata_root / "values.raw"
        metadata.write_bytes(bytes(45))
        before = self.digest(metadata)
        self.refused(self.invoke("--metadata-binary", metadata, metadata_root / "output", self.input))
        self.assertEqual(self.digest(metadata), before)
        self.assertFalse((metadata_root / "output").exists())

    def test_invalid_options_create_no_output(self):
        for options in (("--quality", "../low"), ("--geometry", "128x24junk"),
                        ("--metadata-binary", "")):
            self.refused(self.invoke(*options, self.output, self.input))
            self.no_run()

    def test_valid_sequence_publishes_complete_trace_and_samples(self):
        result = self.invoke("--reference-yuv", self.output, self.input, self.input)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        document = json.loads((self.output / "trace.json").read_text())
        self.assertEqual(document["profile"], "filmscan3")
        self.assertEqual([frame["frame_index"] for frame in document["frames"]], [0, 1])
        for frame in document["frames"]:
            packet = self.output / ("frame_%04d.cfhd" % frame["frame_index"])
            self.assertEqual(packet.stat().st_size, frame["sample_bytes"])
        self.assertEqual(len(list(self.output.iterdir())), 5)
        self.assertFalse(list(self.root.glob(".hypercine-incomplete-*")))
        self.assertEqual(self.digest(self.input), self.input_hash)

    def test_valid_parent_alias_is_pinned_and_works(self):
        real = self.root / "real-output-parent"
        real.mkdir()
        alias = self.root / "parent-alias"
        self.symlink(alias, real)
        result = self.invoke(alias / "result", self.input)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertTrue((real / "result" / "trace.json").is_file())

    def test_two_processes_cannot_replace_each_others_output(self):
        command = [str(ENCODER), "--geometry", "128x24", str(self.output), str(self.input)]
        processes = [subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                     for _ in range(2)]
        for process in processes:
            self.addCleanup(self.stop, process)
        results = [process.communicate(timeout=30) for process in processes]
        self.assertEqual(sorted(process.returncode == 0 for process in processes), [False, True], results)
        trace = json.loads((self.output / "trace.json").read_text())
        self.assertEqual(len(trace["frames"]), 1)
        self.assertEqual((self.output / "frame_0000.cfhd").stat().st_size,
                         trace["frames"][0]["sample_bytes"])
        self.assertEqual(self.digest(self.input), self.input_hash)

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=10)

    @unittest.skipIf(os.name == "nt", "native write-failure scenario is covered by the C++ transaction gate")
    def test_partial_write_failure_never_publishes_output(self):
        import resource

        def restrict_output():
            resource.setrlimit(resource.RLIMIT_FSIZE, (16, 16))
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)

        self.refused(self.invoke("--reference-yuv", self.output, self.input,
                                 preexec_fn=restrict_output))
        self.assertFalse(self.output.exists())
        stages = list(self.root.glob(".hypercine-incomplete-*"))
        self.assertEqual(len(stages), 1)
        self.assertFalse((stages[0] / "trace.json").exists())
        self.assertEqual((stages[0] / "frame_0000.reference.yuv").stat().st_size, 16)

    @unittest.skipIf(os.name == "nt", "Windows input sharing locks prevent this POSIX truncation scenario")
    def test_mid_sequence_input_failure_never_publishes_partial_run(self):
        first = self.inputs / "large-first.bgra"
        later = self.inputs / "large-later.bgra"
        data = bytes((12, 34, 56, 255)) * (1920 * 1080)
        first.write_bytes(data)
        later.write_bytes(data)
        original_first = self.digest(first)
        command = [str(ENCODER), "--geometry", "1920x1080", str(self.output),
                   str(first), str(first), str(first), str(later)]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop, process)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            stages = list(self.root.glob(".hypercine-incomplete-*"))
            if stages and (stages[0] / "frame_0000.cfhd").is_file():
                os.kill(process.pid, signal.SIGSTOP)
                _, status = os.waitpid(process.pid, os.WUNTRACED)
                self.assertTrue(os.WIFSTOPPED(status), "encoder finished before mutation boundary")
                break
            self.assertIsNone(process.poll(), "encoder finished before mutation boundary")
            time.sleep(0.001)
        else:
            self.fail("encoder did not reach its first staged frame")
        later.write_bytes(b"truncated after preflight")
        os.kill(process.pid, signal.SIGCONT)
        _, error = process.communicate(timeout=30)
        self.assertNotEqual(process.returncode, 0, error.decode(errors="replace"))
        self.assertIn(b"truncated", error)
        self.assertFalse(self.output.exists(), "mid-sequence failure published partial output")
        self.assertTrue((stages[0] / "frame_0000.cfhd").is_file())
        self.assertFalse((stages[0] / "trace.json").exists())
        self.assertEqual(self.digest(first), original_first)
        self.assertEqual(self.digest(self.input), self.input_hash)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoder", required=True, type=Path)
    args, rest = parser.parse_known_args()
    ENCODER = args.encoder.resolve()
    unittest.main(argv=[__file__, *rest], verbosity=2)
