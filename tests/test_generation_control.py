# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location("control", Path(__file__).resolve().parents[1] / "tools/measure_cfhd_generations.py")
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)
COUNTS = (4, 2, 2)


def pack(values):
    return struct.pack("<8H", *values)


def mapped(fn):
    return lambda raw, generation: (pack([fn(v) for v in struct.unpack("<8H", raw)]), {})


class GenerationControls(unittest.TestCase):
    def test_identity(self):
        r = m.run_chain(pack(range(8)), mapped(lambda v: v), 5, COUNTS)
        self.assertTrue(r["initial_lossless"])
        self.assertTrue(r["no_extra_loss_in_measured_generations"])
        self.assertEqual(r["rows"][-1]["versus_original"]["Y"]["psnr_db"], "Infinity")

    def test_idempotent_lossy_quantizer(self):
        r = m.run_chain(pack(range(8)), mapped(lambda v: v // 4 * 4), 5, COUNTS)
        self.assertFalse(r["initial_lossless"])
        self.assertTrue(r["no_extra_loss_in_measured_generations"])
        self.assertEqual(r["observed_final_stable_since_generation"], 1)

    def test_drift_chained_inputs_not_original(self):
        received = []
        def step(raw, generation):
            received.append(struct.unpack("<8H", raw)[0])
            return mapped(lambda v: v + 1)(raw, generation)
        r = m.run_chain(pack([10]*8), step, 5, COUNTS)
        self.assertEqual(received, [10,11,12,13,14])
        self.assertFalse(r["no_extra_loss_in_measured_generations"])
        self.assertEqual(r["rows"][-1]["versus_first"]["Y"]["max_abs_error"], 4)
        self.assertIsNone(r["observed_final_stable_since_generation"])

    def test_cycle_not_fixed_point(self):
        r = m.run_chain(pack(range(8)), mapped(lambda v: 100 - v), 5, COUNTS)
        self.assertFalse(r["no_extra_loss_in_measured_generations"])
        self.assertEqual(r["rows"][1]["earlier_equal_generation"], 0)
        self.assertIsNone(r["observed_final_stable_since_generation"])

    def test_metrics_signed_bias_and_changed_count(self):
        r = m.plane_metrics([0,2,4], [1,2,2])
        self.assertEqual(r["changed_samples"], 2)
        self.assertEqual(r["max_abs_error"], 2)
        self.assertAlmostEqual(r["mse"], 5/3)
        self.assertAlmostEqual(r["signed_bias"], -1/3)

    def test_reject_invalid_output(self):
        for raw in (b"", pack([1024]*8), b"\0"*17):
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                m.run_chain(pack(range(8)), lambda a,b:(raw,{}), 2, COUNTS)

    def test_channel_swap_visible(self):
        source = pack([20]*4 + [30]*2 + [80]*2)
        r = m.compare_frames(source, pack([20]*4 + [80]*2 + [30]*2), COUNTS)
        self.assertEqual(r["Y"]["mse"], 0)
        self.assertEqual(r["U"]["max_abs_error"], 50)
        self.assertEqual(r["V"]["signed_bias"], -50)

    def test_one_generation_not_extra_loss_proof(self):
        r = m.run_chain(pack(range(8)), mapped(lambda v:v), 1, COUNTS)
        self.assertIsNone(r["no_extra_loss_in_measured_generations"])
        for n in (0, 51, True):
            with self.assertRaises(ValueError):
                m.run_chain(pack(range(8)), mapped(lambda v:v), n, COUNTS)


if __name__ == "__main__":
    unittest.main()
