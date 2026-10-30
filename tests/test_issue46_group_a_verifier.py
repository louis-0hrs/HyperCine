#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Adversarial, corpus-free tests for the opt-in Group A SDK verifier."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "verify_issue46_group_a", ROOT / "tools" / "verify_issue46_group_a.py")
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


def frame(index=0, previous=0, size=100):
    return {
        "frame_index": index,
        "previous_sdk_sample_bytes": previous,
        "sdk_byte_length": size,
        "packet_band_quant_scale": [
            {"channel": channel, "subband": subband,
             "quant_53": 0, "scale_54": 0}
            for channel in range(3) for subband in range(1, 10)],
    }


def record(index=0):
    return {"frame_index": index, "quant": [0] * 36, "scale": [0] * 36}


def sequence():
    return [frame(i, 0 if i == 0 else 100) for i in range(120)]


class FailClosedGroupA(unittest.TestCase):
    def assert_rejected(self, action):
        with self.assertRaises(SystemExit) as caught:
            action()
        self.assertIn("FAIL:", str(caught.exception))

    def test_valid_sequence_and_tables(self):
        frames = sequence()
        verifier.validate_frames(frames, "low")
        result = verifier.compare([record(0)], [frames[0]])
        self.assertEqual(result["frames"], 0)
        self.assertEqual(result["frames_missing_tables"], 0)

    def test_truncated_replay_must_not_zip_pass(self):
        self.assert_rejected(lambda: verifier.compare([], [frame()]))

    def test_reordered_or_missing_indices(self):
        frames = sequence()
        frames[1]["frame_index"] = 2
        self.assert_rejected(lambda: verifier.validate_frames(frames, "low"))

    def test_broken_previous_packet_chain(self):
        frames = sequence()
        frames[1]["previous_sdk_sample_bytes"] = 0
        self.assert_rejected(lambda: verifier.validate_frames(frames, "low"))

    def test_missing_all_tables(self):
        f = frame()
        f["packet_band_quant_scale"] = []
        self.assert_rejected(lambda: verifier.compare([record()], [f]))

    def test_missing_one_highpass_subband(self):
        f = frame()
        f["packet_band_quant_scale"].pop()
        self.assert_rejected(lambda: verifier.compare([record()], [f]))

    def test_duplicate_highpass_subband(self):
        f = frame()
        f["packet_band_quant_scale"].append(dict(f["packet_band_quant_scale"][0]))
        self.assert_rejected(lambda: verifier.compare([record()], [f]))

    def test_missing_quant_and_scale_tags(self):
        for field in ("quant_53", "scale_54"):
            with self.subTest(field=field):
                f = frame()
                f["packet_band_quant_scale"][0][field] = None
                self.assert_rejected(lambda: verifier.compare([record()], [f]))

    def test_replay_frame_id_mismatch(self):
        self.assert_rejected(lambda: verifier.compare([record(1)], [frame(0)]))

    def test_invalid_packet_byte_size(self):
        frames = sequence()
        frames[1]["sdk_byte_length"] = 0
        self.assert_rejected(lambda: verifier.validate_frames(frames, "low"))


if __name__ == "__main__":
    unittest.main()
