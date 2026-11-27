#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Exercise quality-state replay with synthetic manifests and controlled probe output."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import call, patch

import validate_quality_state_corpus as corpus


SUITES = {
    "1080p-16f": (16, 1920, 1080),
    "sequence-120f": (120, 1920, 1080),
    "3840x2160-1f": (1, 3840, 2160),
    "7680x4320-1f": (1, 7680, 4320),
}
ARTIFACT_SHA256 = "a" * 64


def make_manifest(suite):
    frame_count, width, height = SUITES[suite]
    return {
        "schema_version": 1,
        "profile": {"quality_sdk_enum": 6, "gop": 1, "precision": 10},
        "artifacts": [{"path": "sample.cfhd", "byte_length": 80, "sha256": ARTIFACT_SHA256}],
        "frames": [
            {
                "frame_index": index,
                "frame_order": index,
                "width": width,
                "height": height,
                "previous_sample_bytes": 80 + index if index else 0,
                "fs_rate_limiter": (index // 4) % 3,
                "quant": [1 + (index + band) % 32 for band in range(36)],
                "scale": [1 << (band % 7) for band in range(36)],
            }
            for index in range(frame_count)
        ],
    }


def probe_state(frame):
    return {
        "limiter": frame["fs_rate_limiter"],
        "overbitrate": (frame["frame_index"] + 1) % 8,
        "quant": frame["quant"],
        "scale": frame["scale"],
    }


class QualityStateCorpusReplay(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name).resolve()
        self.manifest_path = self.root / "manifest.json"
        self.probe = self.root / "quality_state_probe"

    def replay(self, manifest, suite="1080p-16f"):
        self.manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        records = {f"filmscan3/{suite}/sample.cfhd": (80, ARTIFACT_SHA256)}
        return corpus.replay_manifest(self.manifest_path, "filmscan3", 6, suite, records, self.probe)

    def test_valid_suites_replay_with_ordered_probe_state(self):
        for suite, (count, width, height) in SUITES.items():
            with self.subTest(suite=suite):
                manifest = make_manifest(suite)
                states = [probe_state(frame) for frame in manifest["frames"]]
                with patch.object(corpus.subprocess, "check_output",
                                  side_effect=[json.dumps(state) for state in states]) as probe:
                    self.assertEqual(self.replay(manifest, suite), (count, (count - 1) // 4))

                expected_calls = []
                for index, frame in enumerate(manifest["frames"]):
                    previous_state = states[index - 1] if index else {"limiter": 0, "overbitrate": 0}
                    arguments = [6, width, height, index, previous_state["limiter"],
                                 frame["previous_sample_bytes"], previous_state["overbitrate"]]
                    expected_calls.append(call([str(self.probe), *map(str, arguments)], text=True))
                self.assertEqual(probe.call_args_list, expected_calls)

    def test_malformed_manifest_shape_refused_before_probe(self):
        valid = make_manifest("1080p-16f")
        profiles = [
            None,
            [],
            {**valid["profile"], "quality_sdk_enum": 6.0},
            {**valid["profile"], "gop": True},
            {**valid["profile"], "precision": 10.0},
        ]
        manifests = [None, [], {**valid, "frames": {}}, {**valid, "frames": "x" * 16},
                     {**valid, "frames": valid["frames"][:-1]}]
        manifests.extend({**valid, "profile": profile} for profile in profiles)
        artifact = valid["artifacts"][0]
        manifests.extend({**valid, "artifacts": artifacts} for artifacts in (
            {}, [None], [{**artifact, "byte_length": 80.0}], [{**artifact, "sha256": "b" * 64}],
        ))
        for index, manifest in enumerate(manifests):
            with self.subTest(case=index), patch.object(corpus.subprocess, "check_output") as probe:
                with self.assertRaises(ValueError):
                    self.replay(manifest)
                probe.assert_not_called()

    def assert_bad_frame_stops_replay(self, manifest, suite, valid_probe_outputs):
        with patch.object(corpus.subprocess, "check_output", side_effect=valid_probe_outputs) as probe:
            with self.assertRaises(ValueError):
                self.replay(manifest, suite)
            probe.assert_not_called()

    def test_entire_sequence_is_validated_before_first_probe(self):
        changes = [
            ("frame_index", 0),
            ("frame_index", True),
            ("width", 1920.0),
            ("width", 3840),
            ("height", "1080"),
            ("height", 2160),
            ("previous_sample_bytes", True),
            ("previous_sample_bytes", -1),
            ("previous_sample_bytes", 2**31),
            ("fs_rate_limiter", 0.0),
            ("fs_rate_limiter", -1),
            ("quant", [1] * 35),
            ("scale", [True] * 36),
        ]
        for suite in ("1080p-16f", "sequence-120f"):
            valid = make_manifest(suite)
            frames = valid["frames"]
            probe_outputs = [json.dumps(probe_state(frame)) for frame in frames]
            for field, value in changes:
                with self.subTest(suite=suite, field=field, value=value):
                    last = {**frames[-1], field: value}
                    manifest = {**valid, "frames": [*frames[:-1], last]}
                    self.assert_bad_frame_stops_replay(manifest, suite, probe_outputs)

            for case, tail in (("non-object", [None]), ("transposed", [frames[-1], frames[-2]])):
                with self.subTest(suite=suite, case=case):
                    manifest = {**valid, "frames": frames[:-len(tail)] + tail}
                    self.assert_bad_frame_stops_replay(manifest, suite, probe_outputs)

    def test_probe_schema_refuses_malformed_state(self):
        manifest = make_manifest("1080p-16f")
        valid = probe_state(manifest["frames"][0])
        changes = [
            ("limiter", False),
            ("limiter", 0.0),
            ("limiter", -1),
            ("overbitrate", True),
            ("overbitrate", 1.0),
            ("overbitrate", -1),
            ("quant", valid["quant"][:-1]),
            ("quant", [True, *valid["quant"][1:]]),
            ("quant", [1.0, *valid["quant"][1:]]),
            ("scale", valid["scale"][:-1]),
            ("scale", [1.0, *valid["scale"][1:]]),
            ("scale", "1" * 36),
        ]
        responses = [None, [], {key: value for key, value in valid.items() if key != "overbitrate"}]
        responses.extend({**valid, field: value} for field, value in changes)
        tail = [json.dumps(probe_state(frame)) for frame in manifest["frames"][1:]]
        for index, response in enumerate(responses):
            with self.subTest(case=index):
                with patch.object(corpus.subprocess, "check_output",
                                  side_effect=[json.dumps(response), *tail]) as probe:
                    with self.assertRaisesRegex(ValueError, "probe"):
                        self.replay(manifest)
                    self.assertEqual(probe.call_count, 1)

    def test_well_formed_probe_mismatch_still_fails_replay(self):
        manifest = make_manifest("1080p-16f")
        result = probe_state(manifest["frames"][0])
        result["scale"] = [2, *result["scale"][1:]]
        with patch.object(corpus.subprocess, "check_output", return_value=json.dumps(result)) as probe:
            with self.assertRaises(ValueError):
                self.replay(manifest)
            self.assertEqual(probe.call_count, 1)


if __name__ == "__main__":
    unittest.main()
