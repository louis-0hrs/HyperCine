# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Independent ledger checks against CLI-emitted packets and real inspector output."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "vc5_synthetic_packet.py"
INSPECTOR = ROOT / "tools" / "vc5_inspect.py"
spec = importlib.util.spec_from_file_location("synthetic_packet", GENERATOR)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
EXPECTED = {"framing": "VC-5", "ChannelCount": 3, "Width": 48, "Height": 48}
# Pinned independently of the generator module: removing an entry from
# generator.SOURCE_FILES must fail closed instead of silently shrinking the ledger.
PINNED_SOURCE_FILES = (
    "CMakeLists.txt",
    "cmake/vc5_contract_inventory.cmake",
    "ci/run_standalone.py",
    "tests/test_vc5_synthetic_ledger.py",
    "tools/vc5_inspect.py",
    "tools/vc5_synthetic_packet.py",
)

def parse_packet(data):
    if data[:4] != b"VC-5":
        raise ValueError("framing")
    fields = {}
    offset = 4
    chunk = None
    while offset < len(data):
        tag = int.from_bytes(data[offset:offset + 2], "big")
        value = int.from_bytes(data[offset + 2:offset + 4], "big")
        offset += 4
        if tag == 0x6000:
            chunk = {"tag": "0x6000", "payload_bytes": value * 4}
            break
        fields[tag] = value
    return {"framing": "VC-5", "ChannelCount": fields[12],
            "Width": fields[20], "Height": fields[21],
            "PrescaleShift": fields[109], "chunk": chunk}

def run_json(command, timeout=8):
    result = subprocess.run(command, check=True, capture_output=True, timeout=timeout)
    return json.loads(result.stdout)

def context_for(ledger, interpreter=sys.executable):
    source = ledger["source"]
    executable_hash = hashlib.sha256(Path(interpreter).read_bytes()).hexdigest()
    return {"claim": "vc5", "acceptance_claim": "encoder", "parts": [1, 3],
            "build_parts": [1, 3], "runtime_parts": [1, 3],
            "source": {"commit": source["commit"],
                       "tree": source["committed_tree"], "dirty": source["dirty"],
                       "build_sha256": ledger["build"]["manifest_sha256"],
                       "executable_sha256": executable_hash},
            "decoder": {"state": "not_run", "qualified": False,
                         "executable_sha256": None, "version_id": None,
                         "configuration_sha256": None, "warnings": 0, "errors": 0,
                         "exit_code": None, "pixel_format": "unknown",
                         "output_sha256": None, "materials_available": False,
                         "inventory_complete": False, "pixels_equal": False}}

class SyntheticLedgerTests(unittest.TestCase):
    def cli_artifacts(self):
        ledger = run_json([sys.executable, "-B", str(GENERATOR)])
        packet = subprocess.run([sys.executable, "-B", str(GENERATOR), "--packet"],
                                check=True, capture_output=True, timeout=8).stdout
        return ledger, packet

    def isolated_root(self, temp):
        """Private copy carrying the current working source, no extra commits."""
        root = Path(temp) / "repo"
        subprocess.run(["git", "clone", "--shared", "--no-checkout", "--quiet",
                        str(ROOT), str(root)],
                       check=True, capture_output=True, timeout=30)
        for name in PINNED_SOURCE_FILES:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        return root

    def run_selected(self, root, name, optimized):
        command = [sys.executable, "-B"] + (["-O"] if optimized else []) + [
            "tests/test_vc5_synthetic_ledger.py", name]
        return subprocess.run(command, cwd=str(root), capture_output=True,
                              text=True, timeout=60)

    def test_cli_ledger_packet_and_field_expectations(self):
        ledger, data = self.cli_artifacts()
        self.assertEqual(ledger["schema"], "hypercine.vc5-synthetic-ledger/1")
        self.assertEqual(ledger["standard"], {"edition": "ST 2073-1:2017", "part": 1,
                         "feature_set": "elementary header/chunk framing subset"})
        self.assertEqual(ledger["conformance"], "BLOCKED_REFERENCE")
        self.assertEqual(ledger["packet"]["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(ledger["packet"]["bytes"], len(data))
        parsed = parse_packet(data)
        for row in ledger["expectations"]:
            name = row["field"]
            if name in EXPECTED:
                self.assertEqual(row["expected"], EXPECTED[name], name)
                self.assertEqual(parsed[name], row["expected"], name)
            elif name == "framing":
                self.assertEqual(row["expected"], EXPECTED[name])
                self.assertEqual(parsed[name], row["expected"])
            elif name == "chunk":
                self.assertEqual(parsed[name], {"tag": row["tag"],
                                 "payload_bytes": row["payload_bytes"]})
            elif name == "PrescaleShift":
                self.assertEqual(parsed[name], row["raw"])
                self.assertEqual(row["status"], "UNQUALIFIED")
                self.assertEqual(row["reason"], "UNRESOLVED_PACKING_INTERPRETATION")
                self.assertEqual(row["table_b1"], [0, 0, 8])
                self.assertEqual(row["reference_layout"], [0, 2, 2])
        self.assertEqual({r["field"] for r in ledger["expectations"]},
                         {"framing", "ChannelCount", "Width", "Height", "chunk", "PrescaleShift"})
        self.assertEqual(ledger["source"]["manifest_sha256"],
                         ledger["build"]["manifest_sha256"])
        self.assertEqual(ledger["source"]["files"], generator.working_manifest()[0])
        self.assertEqual(ledger["source"]["dirty"], bool(subprocess.check_output(
            ["git", "-C", str(ROOT), "status", "--porcelain"], text=True, timeout=3).strip()))
        self.assertEqual(ledger["build"]["python_executable_sha256"],
                         hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest())

    def test_manifest_changes_with_working_source_bytes(self):
        self.assertEqual(len(generator.SOURCE_FILES), len(PINNED_SOURCE_FILES))
        self.assertEqual(set(generator.SOURCE_FILES), set(PINNED_SOURCE_FILES))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in PINNED_SOURCE_FILES:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, target)
            original = generator.working_manifest(root)[1]
            for name in PINNED_SOURCE_FILES:
                target = root / name
                before = target.read_bytes()
                target.write_bytes(before + b"\n# manifest mutation\n")
                self.assertNotEqual(generator.working_manifest(root)[1], original, name)
                target.write_bytes(before)

    def test_source_inventory_is_pinned_independently(self):
        ledger, _ = self.cli_artifacts()
        files = ledger["source"]["files"]
        pinned = set(PINNED_SOURCE_FILES)
        advertised = set(generator.SOURCE_FILES)
        self.assertEqual(advertised, pinned, "generator inventory missing=%s extra=%s"
                         % (sorted(pinned - advertised), sorted(advertised - pinned)))
        self.assertEqual(len(generator.SOURCE_FILES), len(PINNED_SOURCE_FILES))
        self.assertEqual(set(files), pinned, "ledger inventory missing=%s extra=%s"
                         % (sorted(pinned - set(files)), sorted(set(files) - pinned)))
        for name in PINNED_SOURCE_FILES:
            self.assertEqual(files[name],
                             hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), name)

    def test_real_inspector_source_and_packet_identity(self):
        ledger, data = self.cli_artifacts()
        context = context_for(ledger)
        with tempfile.TemporaryDirectory() as temp:
            packet_path = Path(temp) / "packet"
            context_path = Path(temp) / "context.json"
            packet_path.write_bytes(data)
            context_path.write_text(json.dumps(context), encoding="utf-8")
            result = subprocess.run([sys.executable, "-B", str(INSPECTOR),
                                     "--input", str(packet_path), "--context", str(context_path)],
                                    capture_output=True, timeout=8)
        self.assertEqual(result.returncode, 2, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["source"]["commit"], ledger["source"]["commit"])
        self.assertEqual(report["source"]["tree"], ledger["source"]["committed_tree"])
        self.assertEqual(report["source"]["dirty"], ledger["source"]["dirty"])
        self.assertEqual(report["source"]["build_sha256"], ledger["build"]["manifest_sha256"])
        self.assertEqual(report["source"]["executable_sha256"],
                         ledger["build"]["python_executable_sha256"])
        self.assertEqual(report["inspector_sha256"], hashlib.sha256(INSPECTOR.read_bytes()).hexdigest())
        self.assertEqual(report["input"], {"sha256": ledger["packet"]["sha256"],
                                           "bytes": ledger["packet"]["bytes"]})
        self.assertEqual(report["syntax"]["status"], "PASS")
        self.assertEqual(report["syntax"]["prescale"]["table_b1"], [0, 0, 8])
        self.assertEqual(report["syntax"]["prescale"]["reference_layout"], [0, 2, 2])
        self.assertEqual(report["verdict"], "BLOCKED")

    def test_registration_and_cmake_source_removal_guards(self):
        runner_path = ROOT / "ci" / "run_standalone.py"
        sys.path.insert(0, str(ROOT / "ci"))
        try:
            runner_spec = importlib.util.spec_from_file_location("standalone_runner", runner_path)
            runner = importlib.util.module_from_spec(runner_spec)
            runner_spec.loader.exec_module(runner)
        finally:
            sys.path.pop(0)
        required = set(runner.REQUIRED)
        guarded = {"test_vc5_synthetic_ledger", "test_vc5_synthetic_ledger_optimized"}
        self.assertTrue(guarded <= required)
        document = {"tests": [{"name": name} for name in required]}
        runner.registered_tests(document, required)
        for removed in guarded:
            reduced = {"tests": [test for test in document["tests"] if test["name"] != removed]}
            with self.subTest(removed=removed), self.assertRaisesRegex(ValueError, "missing/duplicate CTest registrations"):
                runner.registered_tests(reduced, required)

        guard = ROOT / "cmake" / "vc5_contract_inventory.cmake"
        with tempfile.TemporaryDirectory() as temp:
            source_root = Path(temp) / "source"
            guarded_files = ("tests/test_vc5_inspect.py", "tests/test_vc5_inspect_mutations.py",
                             "tests/test_vc5_synthetic_ledger.py", "tools/vc5_inspect.py",
                             "tools/vc5_synthetic_packet.py")
            for name in guarded_files:
                target = source_root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("fixture\n", encoding="utf-8")
            driver = Path(temp) / "guard.cmake"
            driver.write_text('include("' + guard.as_posix() + '")\n' +
                              'hypercine_require_vc5_contract_files("' + source_root.as_posix() + '")\n',
                              encoding="utf-8")
            good = subprocess.run(["cmake", "-P", str(driver)], capture_output=True, text=True, timeout=10)
            self.assertEqual(good.returncode, 0, good.stderr)
            for missing in ("tests/test_vc5_synthetic_ledger.py", "tools/vc5_synthetic_packet.py"):
                target = source_root / missing
                target.unlink()
                with self.subTest(missing=missing):
                    refusal = subprocess.run(["cmake", "-P", str(driver)], capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(refusal.returncode, 0)
                    self.assertIn("Required VC-5 contract source missing: " + missing, refusal.stderr)
                target.write_text("fixture\n", encoding="utf-8")

    def test_named_source_mutants_are_caught(self):
        mutations = {
            "wrong_generator_header": ("tools/vc5_synthetic_packet.py",
                'segment(12, 3)', 'segment(12, 2)', "ChannelCount"),
            "wrong_ledger_width": ("tools/vc5_synthetic_packet.py",
                '"field": "Width", "tag": 20, "expected": 48',
                '"field": "Width", "tag": 20, "expected": 49', "Width"),
            "wrong_edition": ("tools/vc5_synthetic_packet.py",
                '"edition": "ST 2073-1:2017"',
                '"edition": "ST 2073-1:2022"', "edition"),
            "collapsed_prescale": ("tools/vc5_inspect.py",
                '(14, 12, 10)', '(0, 4, 8)', "PrescaleShift"),
            "wrong_input_identity": ("tools/vc5_inspect.py",
                "'input': {'sha256': hashlib.sha256(data).hexdigest()",
                "'input': {'sha256': '0'*64", "input identity"),
        }
        for mutant, (relative, before, after, assertion_name) in mutations.items():
            with self.subTest(mutant=mutant), tempfile.TemporaryDirectory() as temp:
                root = Path(temp) / "repo"
                # Reuse immutable committed objects, but give every arm its own
                # metadata and working files. No temporary add/commit or hooks
                # are needed to measure the mutated working-source manifest.
                # Capture pipes so the bounded command waits for its output
                # handles before TemporaryDirectory can remove the fixture.
                subprocess.run(["git", "clone", "--shared", "--no-checkout",
                                "--quiet", str(ROOT), str(root)],
                               check=True, capture_output=True, timeout=30)
                for name in generator.SOURCE_FILES:
                    target = root / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(ROOT / name, target)

                def assert_artifact():
                    gen = root / "tools/vc5_synthetic_packet.py"
                    inspector_path = root / "tools/vc5_inspect.py"
                    ledger = run_json([sys.executable, "-B", str(gen)])
                    data = subprocess.run([sys.executable, "-B", str(gen), "--packet"],
                                          check=True, capture_output=True, timeout=8).stdout
                    parsed = parse_packet(data)
                    for field, value in EXPECTED.items():
                        self.assertEqual(parsed[field], value, field)
                    rows = {row["field"]: row for row in ledger["expectations"]}
                    self.assertEqual(ledger["standard"]["edition"], "ST 2073-1:2017", "edition")
                    self.assertEqual(rows["Width"]["expected"], 48, "Width")
                    self.assertEqual(ledger["packet"]["sha256"], hashlib.sha256(data).hexdigest())
                    ctx = context_for(ledger)
                    with tempfile.TemporaryDirectory() as nested:
                        packet_path = Path(nested) / "packet"
                        context_path = Path(nested) / "context.json"
                        packet_path.write_bytes(data)
                        context_path.write_text(json.dumps(ctx), encoding="utf-8")
                        result = subprocess.run([sys.executable, "-B", str(inspector_path),
                            "--input", str(packet_path), "--context", str(context_path)],
                            capture_output=True, timeout=8)
                    report = json.loads(result.stdout)
                    self.assertEqual(report["input"]["sha256"], ledger["packet"]["sha256"], "input identity")
                    p = report["syntax"]["prescale"]
                    self.assertEqual(p["table_b1"], [0, 0, 8], "PrescaleShift")
                    self.assertEqual(p["reference_layout"], [0, 2, 2], "PrescaleShift")

                assert_artifact()  # each isolated unmutated copy must be green
                source = root / relative
                text = source.read_text(encoding="utf-8")
                self.assertIn(before, text, mutant)
                source.write_text(text.replace(before, after, 1), encoding="utf-8")
                with self.assertRaisesRegex(AssertionError, assertion_name):
                    assert_artifact()

    def test_negative_framing_tag_truncation_and_ambiguity(self):
        _, packet = self.cli_artifacts()
        cases = [("wrong framing", b"NOPE" + packet[4:], "UNKNOWN_FRAMING", "FAIL", 1),
                 ("invalid header tag", packet[:8] + b"\x03\xe7\x00\x01" + packet[12:],
                  "HEADER_TAG", "UNQUALIFIED", 3),
                 ("truncated chunk", packet[:-1], "TRUNCATED_SEGMENT", "FAIL", 1)]
        for label, data, reason, verdict, expected_exit in cases:
            with self.subTest(case=label), tempfile.TemporaryDirectory() as temp:
                ledger, _ = self.cli_artifacts()
                packet_path = Path(temp) / "packet"
                context_path = Path(temp) / "context.json"
                packet_path.write_bytes(data)
                context_path.write_text(json.dumps(context_for(ledger)), encoding="utf-8")
                result = subprocess.run([sys.executable, "-B", str(INSPECTOR),
                                         "--input", str(packet_path), "--context", str(context_path)],
                                        capture_output=True, timeout=8)
                report = json.loads(result.stdout)
                self.assertEqual(result.returncode, expected_exit, result.stderr)
                self.assertEqual(report["syntax"]["reason"], reason)
                self.assertEqual(report["verdict"], verdict)
        inspector_module = importlib.util.spec_from_file_location("inspector", INSPECTOR)
        inspector = importlib.util.module_from_spec(inspector_module)
        inspector_module.loader.exec_module(inspector)
        prescale = inspector.inspect_bytes(packet, [1, 3])["prescale"]
        self.assertNotEqual(prescale["table_b1"], prescale["reference_layout"])
        self.assertEqual(prescale["status"], "UNQUALIFIED")

    def test_cli_refusal_exit_is_causally_guarded(self):
        """A FAIL path returning 0 with an unchanged report must be caught."""
        before = "return 1 if report['verdict'] == 'FAIL' else 2"
        after = "return 0 if report['verdict'] == 'FAIL' else 2"
        name = ("SyntheticLedgerTests."
                "test_negative_framing_tag_truncation_and_ambiguity")
        with tempfile.TemporaryDirectory() as temp:
            root = self.isolated_root(temp)
            target = root / "tools" / "vc5_inspect.py"
            original = target.read_bytes()
            for optimized in (False, True):
                mode = "-O" if optimized else "normal"
                baseline = self.run_selected(root, name, optimized)
                self.assertEqual(baseline.returncode, 0,
                                 mode + baseline.stdout + baseline.stderr)
                text = original.decode("utf-8")
                self.assertIn(before, text)
                target.write_text(text.replace(before, after, 1), encoding="utf-8")
                mutant = self.run_selected(root, name, optimized)
                self.assertNotEqual(mutant.returncode, 0, mode + mutant.stdout)
                self.assertIn("0 != 1", mutant.stderr, mode + mutant.stderr)
                target.write_bytes(original)
                restored = self.run_selected(root, name, optimized)
                self.assertEqual(restored.returncode, 0,
                                 mode + restored.stdout + restored.stderr)

    def test_source_inventory_omission_is_causally_guarded(self):
        """Dropping a required path from the generator inventory must fail closed."""
        before = '    "ci/run_standalone.py",\n'
        name = "SyntheticLedgerTests.test_source_inventory_is_pinned_independently"
        with tempfile.TemporaryDirectory() as temp:
            root = self.isolated_root(temp)
            target = root / "tools" / "vc5_synthetic_packet.py"
            original = target.read_bytes()
            for optimized in (False, True):
                mode = "-O" if optimized else "normal"
                baseline = self.run_selected(root, name, optimized)
                self.assertEqual(baseline.returncode, 0,
                                 mode + baseline.stdout + baseline.stderr)
                text = original.decode("utf-8")
                self.assertIn(before, text)
                target.write_text(text.replace(before, "", 1), encoding="utf-8")
                mutant = self.run_selected(root, name, optimized)
                self.assertNotEqual(mutant.returncode, 0, mode + mutant.stdout)
                self.assertIn("missing=['ci/run_standalone.py']", mutant.stderr,
                              mode + mutant.stderr)
                target.write_bytes(original)
                restored = self.run_selected(root, name, optimized)
                self.assertEqual(restored.returncode, 0,
                                 mode + restored.stdout + restored.stderr)

if __name__ == "__main__":
    unittest.main()
