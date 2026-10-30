#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Adversarial, corpus-free tests for the opt-in Group A SDK verifier."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock
import json
import sys
import tempfile

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

    def test_first_absent_packet_normalizes_to_probe_zero(self):
        frames = sequence()
        frames[0]["previous_sdk_sample_bytes"] = None
        verifier.validate_frames(frames, "low")
        payload = '{"limiter":0,"overbitrate":0,"quant":' + str([0]*36) + ',"scale":' + str([0]*36) + '}'
        with mock.patch.object(verifier.subprocess, "check_output", return_value=payload) as call:
            result = verifier.replay(Path("probe"), frames[:2], 1, 1920, 1080, True)
        self.assertEqual(call.call_args_list[0].args[0][6], "0")
        self.assertEqual(call.call_args_list[1].args[0][6], "100")
        self.assertEqual(result[0]["previous_bytes"], 0)

    def test_invalid_first_or_later_previous_bytes(self):
        for index, value in ((0, 1), (0, False), (1, None), (1, 100.0)):
            with self.subTest(index=index, value=value):
                frames = sequence()
                frames[index]["previous_sdk_sample_bytes"] = value
                self.assert_rejected(lambda: verifier.validate_frames(frames, "low"))
        frames = sequence()
        del frames[0]["previous_sdk_sample_bytes"]
        self.assert_rejected(lambda: verifier.validate_frames(frames, "low"))

    def test_optional_lowpass_entries_accept_missing_tags(self):
        f = frame()
        for channel in range(3):
            f["packet_band_quant_scale"].append({"channel": channel, "subband": 0,
                                                "quant_53": None, "scale_54": None})
        self.assertEqual(verifier.compare([record()], [f])["frames"], 0)

    def test_duplicate_optional_lowpass_rejected(self):
        f = frame()
        f["packet_band_quant_scale"] += [{"channel": 0, "subband": 0}] * 2
        self.assert_rejected(lambda: verifier.compare([record()], [f]))

    def test_unexpected_or_noninteger_band_coordinates_rejected(self):
        for channel, subband in ((3, 0), (0, 10), (-1, 0), (True, 0), (0, 1.0)):
            with self.subTest(channel=channel, subband=subband):
                f = frame()
                f["packet_band_quant_scale"].append({"channel": channel, "subband": subband})
                self.assert_rejected(lambda: verifier.compare([record()], [f]))

    def test_untrusted_index_rejected_before_corpus_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("TRANSFER_SHA256.tsv", "manifest.json", "probe"):
                (root / name).write_text("{}")
            argv = ["verify", "--corpus", str(root), "--probe", str(root / "probe")]
            with mock.patch.object(sys, "argv", argv):
                self.assert_rejected(verifier.main)

    def test_incomplete_probe_tables_rejected(self):
        for field in ("quant", "scale"):
            payload = {"limiter": 0, "overbitrate": 0, "quant": [0]*36, "scale": [0]*36}
            payload[field].pop()
            with mock.patch.object(verifier.subprocess, "check_output", return_value=json.dumps(payload)):
                self.assert_rejected(lambda: verifier.replay(Path("probe"), [frame()], 1, 1920, 1080, True))

    def test_boolean_frame_index_rejected(self):
        frames = sequence()
        frames[0]["frame_index"] = False
        self.assert_rejected(lambda: verifier.validate_frames(frames, "low"))

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
