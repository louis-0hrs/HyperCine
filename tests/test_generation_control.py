# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import importlib.util
import contextlib
import io
import runpy
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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


class FFmpegFailureControls(unittest.TestCase):
    """Synthetic process outcomes; no FFmpeg installation or media required."""

    def fixture(self, root):
        executable = root / "synthetic-ffmpeg.bin"
        executable.write_bytes(b"non-executable identity fixture")
        return executable, m.sha256(executable.read_bytes())

    def test_missing_executable_is_refused_before_process(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(m.subprocess, "run") as process:
                with self.assertRaises(OSError):
                    m.FFmpegControl(root / "missing.bin", "0" * 64, root)
                process.assert_not_called()

    def test_wrong_executable_digest_is_refused_before_process(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable, digest = self.fixture(root)
            wrong = ("0" if digest[0] != "0" else "1") + digest[1:]
            with mock.patch.object(m.subprocess, "run") as process:
                with self.assertRaisesRegex(ValueError, "digest mismatch"):
                    m.FFmpegControl(executable, wrong, root)
                process.assert_not_called()

    def test_invalid_executable_digest_is_refused_before_process(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable, _ = self.fixture(root)
            for digest in ("", "0" * 63, "g" * 64):
                with self.subTest(digest_length=len(digest)):
                    with mock.patch.object(m.subprocess, "run") as process:
                        with self.assertRaises(ValueError):
                            m.FFmpegControl(executable, digest, root)
                        process.assert_not_called()

    def test_nonzero_exit_or_stderr_is_not_success(self):
        sentinel = b"CONTROL_PRIVATE_SENTINEL"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable, digest = self.fixture(root)
            control = m.FFmpegControl(executable, digest, root)
            for code, stderr in ((1, b""), (0, sentinel), (1, sentinel)):
                with self.subTest(returncode=code, has_stderr=bool(stderr)):
                    outcome = subprocess.CompletedProcess([], code, stderr=stderr)
                    with mock.patch.object(m.subprocess, "run", return_value=outcome):
                        with self.assertRaises(RuntimeError) as raised:
                            control.command(["synthetic-argument"])
                    self.assertNotIn(sentinel.decode(), str(raised.exception))

    def assert_cli_refusal(self, failing_call, timeout):
        # Execute the actual __main__ exception handler, replacing only the
        # external process boundary. A decode failure must follow one encode.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable, digest = self.fixture(root)
            receipt = root / "receipt.json"
            argv = [m.__file__, "--ffmpeg", str(executable),
                    "--ffmpeg-sha256", digest, "--output", str(receipt),
                    "--case", "flat"]
            sentinel = b"CONTROL_PRIVATE_SENTINEL"
            failure = (subprocess.TimeoutExpired([sentinel.decode()], 8,
                       output=sentinel, stderr=sentinel) if timeout else
                       subprocess.CompletedProcess([], 1, stderr=sentinel))
            outcomes = [subprocess.CompletedProcess([], 0, stderr=b"")
                        for _ in range(failing_call - 1)] + [failure]
            stdout, stderr = io.StringIO(), io.StringIO()
            with mock.patch.object(sys, "argv", argv):
                with mock.patch.object(m.subprocess, "run", side_effect=outcomes) as process:
                    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                        with self.assertRaises(SystemExit) as raised:
                            runpy.run_path(m.__file__, run_name="__main__")
            self.assertEqual(str(raised.exception),
                             "Control failed; no qualification result claimed.")
            self.assertFalse(receipt.exists())
            self.assertEqual(stdout.getvalue(), "")
            self.assertEqual(stderr.getvalue(), "")
            self.assertEqual(process.call_count, failing_call)
            self.assertIn("cfhd", process.call_args_list[0].args[0])
            if failing_call == 2:
                self.assertIn("rawvideo", process.call_args_list[1].args[0])
            for call in process.call_args_list:
                self.assertEqual(call.kwargs["timeout"], 8)
                self.assertEqual(call.kwargs["stdout"], subprocess.DEVNULL)
                self.assertEqual(call.kwargs["stderr"], subprocess.PIPE)
                self.assertIn("-nostdin", call.args[0])
                self.assertIn("-n", call.args[0])

    def test_encode_and_decode_failures_publish_no_receipt(self):
        for failing_call in (1, 2):
            with self.subTest(failing_call=failing_call):
                self.assert_cli_refusal(failing_call, timeout=False)

    def test_encode_and_decode_timeouts_publish_no_receipt(self):
        for failing_call in (1, 2):
            with self.subTest(failing_call=failing_call):
                self.assert_cli_refusal(failing_call, timeout=True)

    def test_real_cli_identity_refusals_are_sanitized(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable, digest = self.fixture(root)
            wrong = ("0" if digest[0] != "0" else "1") + digest[1:]
            for name, selected, pin in (("missing", root / "missing.bin", digest),
                                        ("wrong", executable, wrong)):
                with self.subTest(case=name):
                    receipt = root / (name + "-receipt.json")
                    args = [sys.executable, "-B"]
                    if sys.flags.optimize:
                        args.append("-O")
                    args += [m.__file__, "--ffmpeg", str(selected),
                             "--ffmpeg-sha256", pin, "--output", str(receipt)]
                    result = subprocess.run(args, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, b"")
                    self.assertEqual(result.stderr.strip(),
                                     b"Control failed; no qualification result claimed.")
                    self.assertFalse(receipt.exists())

    def test_executable_change_is_detected_after_experiment(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable, digest = self.fixture(root)
            control = m.FFmpegControl(executable, digest, root)
            control.verify_unchanged()
            executable.write_bytes(b"changed identity fixture")
            with self.assertRaisesRegex(RuntimeError, "executable changed"):
                control.verify_unchanged()


if __name__ == "__main__":
    unittest.main()
