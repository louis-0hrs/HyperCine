#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Synthetic ownership/failure controls, not an original-SDK parity claim."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import p6_writer_parity as gate


class ReadProbe:
    """Real file descriptor with deterministic short reads / producer changes."""
    def __init__(self, source, before_first=None, after_eof=None, chunk=None):
        self.source = source
        self.before_first = before_first
        self.after_eof = after_eof
        self.chunk = chunk
        self.requests = []
        self.returned = 0

    def fileno(self):
        return self.source.fileno()

    def read(self, size=-1):
        if not isinstance(size, int) or size < 0:
            raise AssertionError("unbounded payload read")
        self.requests.append(size)
        if self.before_first is not None:
            callback, self.before_first = self.before_first, None
            callback()
        result = self.source.read(min(size, self.chunk) if self.chunk else size)
        self.returned += len(result)
        if not result and self.after_eof is not None:
            callback, self.after_eof = self.after_eof, None
            callback()
        return result


@contextlib.contextmanager
def probe_reads(name, **options):
    original = gate.Directory.file
    probes = []
    @contextlib.contextmanager
    def controlled(directory, filename, create=False):
        with original(directory, filename, create=create) as source:
            if filename == name and not create:
                probe = ReadProbe(source, **options)
                probes.append(probe)
                yield probe
            else:
                yield source
    with mock.patch.object(gate.Directory, "file", controlled):
        yield probes


class BoundedInputReads(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="p6-read-budget-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.path = self.root / "payload.bin"
        self.payload = bytes(range(16))
        self.path.write_bytes(self.payload)
        self.pin = (len(self.payload), gate.sha_bytes(self.payload))

    def test_verified_bytes_growth_after_fstat_is_bounded(self):
        def grow():
            with self.path.open("ab") as writer:
                writer.write(b"extra" * 20)
        with gate.Directory.open(self.root) as directory, probe_reads(
                self.path.name, before_first=grow) as probes:
            with self.assertRaisesRegex(ValueError, "grew beyond"):
                gate._verified_bytes(directory, self.path.name, self.pin, "synthetic")
        self.assertEqual(probes[0].returned, len(self.payload) + 1)
        self.assertEqual(probes[0].requests, [len(self.payload), 1])

    def test_verified_copy_growth_never_copies_the_overflow_probe(self):
        def grow():
            with self.path.open("ab") as writer:
                writer.write(b"extra" * 20)
        destination = io.BytesIO()
        with gate.Directory.open(self.root) as directory, probe_reads(
                self.path.name, before_first=grow) as probes:
            with self.assertRaisesRegex(ValueError, "grew beyond"):
                gate._verified_file(directory, self.path.name, self.pin, "synthetic", destination)
        self.assertEqual(destination.getvalue(), self.payload)
        self.assertEqual(probes[0].returned, len(self.payload) + 1)

    def test_short_reads_accumulate_to_the_exact_pinned_snapshot(self):
        with gate.Directory.open(self.root) as directory, probe_reads(
                self.path.name, chunk=3) as probes:
            actual = gate._verified_bytes(directory, self.path.name, self.pin, "synthetic")
        self.assertEqual(actual, self.payload)
        self.assertGreater(len(probes[0].requests), 2)
        self.assertEqual(probes[0].returned, len(self.payload))

    def test_short_reads_hash_and_copy_every_admitted_byte(self):
        destination = io.BytesIO()
        with gate.Directory.open(self.root) as directory, probe_reads(self.path.name, chunk=3):
            gate._verified_file(directory, self.path.name, self.pin, "synthetic", destination)
        self.assertEqual(destination.getvalue(), self.payload)

    def test_early_eof_after_fstat_is_rejected(self):
        def shrink():
            with self.path.open("r+b") as writer:
                writer.truncate(5)
        with gate.Directory.open(self.root) as directory, probe_reads(
                self.path.name, before_first=shrink):
            with self.assertRaisesRegex(ValueError, "ended before"):
                gate._verified_bytes(directory, self.path.name, self.pin, "synthetic")

    def test_exact_limit_passes_and_limit_plus_one_fails_before_payload_read(self):
        with gate.Directory.open(self.root) as directory:
            self.assertEqual(directory.read(self.path.name, len(self.payload)), self.payload)
            with self.path.open("ab") as writer:
                writer.write(b"!")
            with probe_reads(self.path.name) as probes:
                with self.assertRaisesRegex(ValueError, "exceeds read limit"):
                    directory.read(self.path.name, len(self.payload))
            self.assertEqual(probes[0].requests, [])

    def test_hash_binds_returned_bytes_when_producer_changes_after_eof(self):
        replacement = bytes(reversed(self.payload))
        with gate.Directory.open(self.root) as directory, probe_reads(
                self.path.name, after_eof=lambda: self.path.write_bytes(replacement)) as probes:
            actual = gate._verified_bytes(directory, self.path.name, self.pin, "synthetic")
        self.assertEqual(actual, self.payload)
        self.assertEqual(gate.sha_bytes(actual), self.pin[1])
        self.assertEqual(self.path.read_bytes(), replacement)
        self.assertEqual(len(probes), 1)

    def test_same_length_hash_mismatch_remains_failure(self):
        self.path.write_bytes(bytes(reversed(self.payload)))
        with gate.Directory.open(self.root) as directory:
            with self.assertRaisesRegex(SystemExit, "hash mismatch"):
                gate._verified_bytes(directory, self.path.name, self.pin, "synthetic")

    def test_compatibility_pinned_helper_stops_at_exact_length_plus_probe(self):
        original_open = Path.open
        probes = []
        def grow():
            with original_open(self.path, "ab") as writer:
                writer.write(b"extra" * 20)
        @contextlib.contextmanager
        def controlled(path, *args, **kwargs):
            with original_open(path, *args, **kwargs) as stream:
                probe = ReadProbe(stream, before_first=grow, chunk=3)
                probes.append(probe)
                yield probe
        index = {self.path.name: self.pin}
        with mock.patch.object(Path, "open", controlled):
            with self.assertRaises(SystemExit) as rejected:
                gate.require_pinned(self.path, *self.pin, index, self.root, "synthetic")
        self.assertEqual(probes[0].returned, len(self.payload) + 1)
        self.assertIn("byte length mismatch", str(rejected.exception))

    def test_compatibility_pinned_helper_accepts_short_reads_and_empty_pin(self):
        original_open = Path.open
        @contextlib.contextmanager
        def controlled(path, *args, **kwargs):
            with original_open(path, *args, **kwargs) as stream:
                yield ReadProbe(stream, chunk=3)
        for payload in (self.payload, b""):
            with self.subTest(length=len(payload)):
                self.path.write_bytes(payload)
                pin = len(payload), gate.sha_bytes(payload)
                with mock.patch.object(Path, "open", controlled):
                    gate.require_pinned(self.path, *pin, {self.path.name: pin}, self.root, "synthetic")


class OutputOwnership(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="p6-ownership-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.corpus = self.root / "corpus"
        self.work = self.root / "work"
        self.report = self.root / "report.json"
        self.base = self.corpus / "filmscan3" / "1080p-16f"
        self.frames = []
        self.packets = []
        for index in range(2):
            folder = self.base / "frames" / "filmscan3-sequence-1920x1080" / ("frame_%04d" % index)
            folder.mkdir(parents=True)
            packet = bytes([index + 1]) * 32
            self.packets.append(packet)
            (folder / "input.bgra").write_bytes(bytes([index + 3]) * 64)
            (folder / "sdk.cfhd").write_bytes(packet)
            self.frames.append(dict(frame_index=index, input_byte_length=64,
                input_sha256=gate.sha_file(folder / "input.bgra"), sdk_byte_length=32,
                sdk_sha256=gate.sha_bytes(packet), metadata_mask_ranges=[],
                pattern="synthetic", quant=[1] * 36, scale=[2] * 36))
        self.update_manifest()
        self.sentinel = self.root / "previous-evidence.bin"
        self.sentinel.write_bytes(b"retained evidence")
        self.args = argparse.Namespace(corpus=self.corpus, work=self.work,
            encoder=self.root / "synthetic-encoder", geometry="1080p-16f",
            profiles="filmscan3", max_frames=2, report=self.report)

    def update_manifest(self):
        (self.base / "manifest.json").write_text(json.dumps({"frames": self.frames}), encoding="utf-8")
        records = ["%s\t%d\t%s" % (p.relative_to(self.corpus).as_posix(), p.stat().st_size, gate.sha_file(p))
                   for p in sorted(self.corpus.rglob("*")) if p.is_file() and p.name != "TRANSFER_SHA256.tsv"]
        (self.corpus / "TRANSFER_SHA256.tsv").write_text("\n".join(records) + "\n", encoding="ascii")

    def snapshot(self):
        return {p: gate.sha_file(p) for p in self.corpus.rglob("*") if p.is_file()}

    def preserved(self, snapshot):
        self.assertEqual(snapshot, self.snapshot())
        self.assertEqual(self.sentinel.read_bytes(), b"retained evidence")

    def emit(self, command, **kwargs):
        output = Path(command[command.index("--quality") + 2])
        output.mkdir()
        frames = []
        for index, packet in enumerate(self.packets):
            (output / ("frame_%04d.cfhd" % index)).write_bytes(packet)
            frames.append(dict(frame_index=index, quant=[1] * 36, scale=[2] * 36,
                               fs_rate_limiter=4, previous_sample_bytes=0))
        (output / "trace.json").write_text(json.dumps({"frames": frames}), encoding="utf-8")
        return mock.Mock(returncode=0, stderr="")

    @contextlib.contextmanager
    def harness(self, emit=None):
        self.stdout, self.stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(gate.subprocess, "run", side_effect=emit or self.emit) as child, \
                mock.patch.object(gate.sample, "parse_sample", return_value={}), \
                mock.patch.object(gate.sample, "normalized_sample", side_effect=lambda data, *args: data), \
                contextlib.redirect_stdout(self.stdout), contextlib.redirect_stderr(self.stderr):
            yield child

    def refused(self, emit=None):
        before = self.snapshot()
        with self.harness(emit) as child:
            with self.assertRaises((SystemExit, ValueError, OSError)):
                gate._run(self.args)
        self.preserved(before)
        self.assertNotIn("p6_writer_parity: PASS", self.stdout.getvalue())
        return child

    def symlink(self, target, link, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError):
            self.skipTest("filesystem does not permit the symlink negative control")

    def test_valid_replay_preserves_corpus_and_uses_separate_inputs(self):
        before = self.snapshot()
        with self.harness() as child:
            self.assertEqual(gate._run(self.args), 0)
        child.assert_called_once()
        report = json.loads(self.report.read_text())
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["failures"], [])
        self.assertTrue((self.work / "filmscan3" / "inputs" / "input_0000.bgra").is_file())
        self.assertTrue((self.work / "filmscan3" / "samples" / "trace.json").is_file())
        self.preserved(before)

    def test_report_inside_new_work_is_supported(self):
        self.args.report = self.work / "report.json"
        with self.harness():
            self.assertEqual(gate._run(self.args), 0)
        self.assertEqual(json.loads(self.args.report.read_text())["status"], "PASS")

    def test_complete_divergence_publishes_explicit_fail_report(self):
        def differing(command, **kwargs):
            done = self.emit(command, **kwargs)
            packet = self.work / "filmscan3" / "samples" / "frame_0001.cfhd"
            packet.write_bytes(b"different" * 4)
            return done
        with self.harness(differing):
            self.assertEqual(gate._run(self.args), 1)
        report = json.loads(self.report.read_text())
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(report["failures"])
        self.assertFalse(report["profiles"]["filmscan3"]["all_normalized_equal"])

    def test_existing_work_directory_and_prior_samples_remain_intact(self):
        old = self.work / "filmscan3" / "samples" / "frame_0000.cfhd"
        old.parent.mkdir(parents=True)
        old.write_bytes(b"previous sample")
        self.refused().assert_not_called()
        self.assertEqual(old.read_bytes(), b"previous sample")

    def test_existing_work_file_is_not_replaced(self):
        self.work.write_bytes(b"previous file")
        self.refused().assert_not_called()
        self.assertEqual(self.work.read_bytes(), b"previous file")

    def test_linked_and_dangling_work_destinations_are_not_followed(self):
        for index, target in enumerate((self.corpus, self.root / "absent")):
            with self.subTest(index=index):
                self.args.work = self.root / ("linked-work-%d" % index)
                self.symlink(target, self.args.work, directory=True)
                self.refused().assert_not_called()
                self.assertTrue(self.args.work.is_symlink())

    def test_work_equal_nested_and_alias_to_corpus_are_rejected(self):
        alias = self.root / "alias"
        self.symlink(self.corpus, alias, directory=True)
        for work in (self.corpus, self.corpus / "new-run", alias / "new-run"):
            with self.subTest(work=work.name):
                self.args.work = work
                self.refused().assert_not_called()
        self.assertFalse((self.corpus / "new-run").exists())

    def test_physical_corpus_ancestor_guard_does_not_depend_on_spelling(self):
        # Simulate a path-spelling comparison that misses a physical alias
        # (case-insensitive APFS or a bind-mount spelling). Real descriptors
        # and inode identities still have to reject the retained ancestor.
        self.args.work = self.corpus / "physical-alias-run"
        with mock.patch.object(gate, "within", return_value=False):
            self.refused().assert_not_called()
        self.assertFalse(self.args.work.exists())

    def test_report_corpus_work_and_reserved_internal_destinations_rejected(self):
        for report in (self.corpus / "new-report.json", self.base / "manifest.json",
                       self.work, self.work / "filmscan3", self.work / "FILMSCAN3",
                       self.work / "filmscan3" / "samples" / "trace.json"):
            with self.subTest(report=report.name):
                self.args.report = report
                self.refused().assert_not_called()
                self.assertFalse(self.work.exists())

    def test_physical_report_ancestor_guard_does_not_depend_on_spelling(self):
        self.args.report = self.corpus / "physical-alias-report.json"
        with mock.patch.object(gate, "within", return_value=False):
            self.refused().assert_not_called()
        self.assertFalse(self.work.exists())

    def test_existing_report_and_report_symlink_are_preserved(self):
        self.report.write_bytes(b"previous report")
        self.refused().assert_not_called()
        self.assertEqual(self.report.read_bytes(), b"previous report")
        linked = self.root / "linked-report.json"
        self.symlink(self.report, linked)
        self.args.report = linked
        self.refused().assert_not_called()
        self.assertEqual(self.report.read_bytes(), b"previous report")

    def test_invalid_selectors_fail_before_any_output_creation(self):
        cases = [("profiles", value) for value in ("", "../filmscan3", "filmscan3,filmscan3", "C:escape", "filmscan3,")]
        cases += [("geometry", value) for value in ("../1080p-16f", "a/b", "a\\b", "C:escape", "CON", "name.")]
        cases += [("max_frames", value) for value in (0, -1)]
        for field, value in cases:
            old = getattr(self.args, field)
            with self.subTest(field=field, value=value):
                setattr(self.args, field, value)
                self.refused().assert_not_called()
                self.assertFalse(self.work.exists())
            setattr(self.args, field, old)

    def test_windows_special_report_components_are_rejected_portably(self):
        for name in ("NUL", "CON.json", "file:stream", "trailing.", "trailing "):
            with self.subTest(name=name):
                self.args.report = self.root / name
                self.refused().assert_not_called()
        self.assertFalse(self.work.exists())

    def test_invalid_manifest_indices_rejected_before_work_creation(self):
        for value in (False, -1, 0.0, "0", 1):
            with self.subTest(value=value):
                self.frames[0]["frame_index"] = value
                self.update_manifest()
                self.refused().assert_not_called()
                self.assertFalse(self.work.exists())

    def test_tampered_original_input_and_sdk_rejected_before_creation(self):
        folder = self.base / "frames" / "filmscan3-sequence-1920x1080" / "frame_0000"
        for name in ("input.bgra", "sdk.cfhd"):
            path = folder / name
            original = path.read_bytes()
            with self.subTest(name=name):
                path.write_bytes(bytes([99]) + original[1:])
                self.refused().assert_not_called()
                self.assertFalse(self.work.exists())
            path.write_bytes(original)

    def test_transfer_path_escape_is_rejected_before_creation(self):
        path = self.corpus / "TRANSFER_SHA256.tsv"
        original = path.read_bytes()
        for rel in ("../escape", "C:escape", "a\\b", "/absolute", "a//b", "NUL"):
            with self.subTest(rel=rel):
                path.write_bytes(original + (rel + "\t1\t" + "0" * 64 + "\n").encode("ascii"))
                self.refused().assert_not_called()
                self.assertFalse(self.work.exists())

    def test_encoder_failure_does_not_publish_or_echo_untrusted_child_text(self):
        def failed(command, **kwargs):
            output = Path(command[command.index("--quality") + 2])
            output.mkdir()
            (output / "frame_0000.cfhd").write_bytes(b"partial owned data")
            return mock.Mock(returncode=17, stderr="status=PASS credential=SYNTHETIC_CHILD_CANARY")
        self.refused(failed).assert_called_once()
        self.assertFalse(self.report.exists())
        self.assertNotIn("SYNTHETIC_CHILD_CANARY", self.stdout.getvalue() + self.stderr.getvalue())

    def test_input_copy_write_failure_preserves_prior_evidence(self):
        original = gate.Directory.file
        @contextlib.contextmanager
        def fail(directory, name, create=False):
            if create and name == "input_0001.bgra":
                raise OSError("injected write failure")
            with original(directory, name, create=create) as stream:
                yield stream
        with mock.patch.object(gate.Directory, "file", fail):
            self.refused().assert_not_called()
        self.assertFalse(self.report.exists())

    def test_partial_report_write_never_publishes_partial_json(self):
        original = gate.Directory.write
        def fail(directory, name, data):
            original(directory, name, data[:len(data) // 2])
            raise OSError("injected partial report write")
        with mock.patch.object(gate.Directory, "write", fail):
            self.refused().assert_called_once()
        self.assertFalse(self.report.exists())

    def test_report_occupied_after_preflight_is_not_replaced(self):
        original = gate.Directory.publish
        def occupy(directory, name, data):
            self.report.write_bytes(b"concurrent report")
            return original(directory, name, data)
        with mock.patch.object(gate.Directory, "publish", occupy):
            self.refused().assert_called_once()
        self.assertEqual(self.report.read_bytes(), b"concurrent report")

    def test_work_occupied_after_preflight_is_not_reused(self):
        original = gate.Directory.child
        def occupy(directory, name, create=False):
            if create and name == "work":
                self.work.mkdir()
                (self.work / "previous.bin").write_bytes(b"concurrent evidence")
            return original(directory, name, create=create)
        with mock.patch.object(gate.Directory, "child", occupy):
            self.refused().assert_not_called()
        self.assertEqual((self.work / "previous.bin").read_bytes(), b"concurrent evidence")

    @unittest.skipIf(os.name == "nt", "Windows open parent handles prevent rename substitution")
    def test_parent_symlink_substitution_during_work_creation_cannot_touch_corpus(self):
        parent = self.root / "runs"
        parent.mkdir()
        self.args.work = parent / "work"
        detached = self.root / "detached-runs"
        original = gate.Directory.child
        def replace(directory, name, create=False):
            if create and name == "work":
                parent.rename(detached)
                self.symlink(self.corpus, parent, directory=True)
            return original(directory, name, create=create)
        with mock.patch.object(gate.Directory, "child", replace):
            self.refused().assert_not_called()
        self.assertFalse((self.corpus / "work").exists())

    def test_report_parent_alias_retargeted_after_preflight_is_detected(self):
        first = self.root / "reports-first"
        second = self.root / "reports-second"
        first.mkdir(); second.mkdir()
        alias = self.root / "reports-alias"
        self.symlink(first, alias, directory=True)
        self.args.report = alias / "report.json"
        def replace(command, **kwargs):
            done = self.emit(command, **kwargs)
            alias.unlink()
            self.symlink(second, alias, directory=True)
            return done
        self.refused(replace).assert_called_once()
        self.assertFalse((first / "report.json").exists())
        self.assertFalse((second / "report.json").exists())

    def test_linked_encoder_output_directory_is_rejected(self):
        def linked(command, **kwargs):
            output = Path(command[command.index("--quality") + 2])
            self.symlink(self.corpus, output, directory=True)
            return mock.Mock(returncode=0, stderr="")
        self.refused(linked).assert_called_once()
        self.assertFalse(self.report.exists())

    def test_linked_encoder_packet_is_rejected(self):
        def linked(command, **kwargs):
            done = self.emit(command, **kwargs)
            output = self.work / "filmscan3" / "samples" / "frame_0000.cfhd"
            output.unlink()
            self.symlink(self.sentinel, output)
            return done
        self.refused(linked).assert_called_once()
        self.assertFalse(self.report.exists())

    def test_concurrent_runs_cannot_share_a_work_directory(self):
        barrier = threading.Barrier(2)
        original = gate._plan
        def synchronized(*args):
            plan = original(*args)
            barrier.wait(timeout=10)
            return plan
        def attempt():
            try:
                return gate._run(self.args)
            except (ValueError, OSError, SystemExit):
                return "refused"
        before = self.snapshot()
        with self.harness(), mock.patch.object(gate, "_plan", synchronized), ThreadPoolExecutor(2) as pool:
            results = list(pool.map(lambda _: attempt(), range(2)))
        self.assertCountEqual(results, [0, "refused"])
        self.assertEqual(json.loads(self.report.read_text())["status"], "PASS")
        self.preserved(before)

    def test_transfer_index_growth_rejected_before_parsing_or_work_creation(self):
        index = self.corpus / "TRANSFER_SHA256.tsv"
        limit = index.stat().st_size
        def grow():
            with index.open("ab") as writer:
                writer.write(b"extra" * 20)
        with mock.patch.object(gate, "MAX_TRANSFER_INDEX_BYTES", limit), \
                probe_reads(index.name, before_first=grow) as probes, \
                mock.patch.object(gate, "parse_transfer_index") as parse, self.harness() as child:
            with self.assertRaisesRegex(ValueError, "grew beyond"):
                gate._run(self.args)
        parse.assert_not_called()
        child.assert_not_called()
        self.assertFalse(self.work.exists())
        self.assertEqual(probes[0].returned, limit + 1)

    def test_transfer_index_parser_and_report_hash_share_the_same_snapshot(self):
        index = self.corpus / "TRANSFER_SHA256.tsv"
        original = index.read_bytes()
        replacement = original + b"later-entry\\t1\\t" + b"0" * 64 + b"\n"
        with probe_reads(index.name, after_eof=lambda: index.write_bytes(replacement)), self.harness():
            self.assertEqual(gate._run(self.args), 0)
        receipt = json.loads(self.report.read_text())["transfer_index"]
        self.assertEqual(receipt["sha256"], gate.sha_bytes(original))
        self.assertEqual(receipt["entries"], len(original.splitlines()))
        self.assertEqual(index.read_bytes(), replacement)

    def test_manifest_growth_after_fstat_rejected_before_json_or_work(self):
        manifest = self.base / "manifest.json"
        length = manifest.stat().st_size
        def grow():
            with manifest.open("ab") as writer:
                writer.write(b" " * 100)
        with probe_reads(manifest.name, before_first=grow) as probes, \
                mock.patch.object(gate.json, "loads") as parse, self.harness() as child:
            with self.assertRaisesRegex(ValueError, "grew beyond"):
                gate._run(self.args)
        parse.assert_not_called()
        child.assert_not_called()
        self.assertFalse(self.work.exists())
        self.assertEqual(probes[0].returned, length + 1)

    def test_transfer_manifest_trace_and_packet_budgets_are_enforced(self):
        for constant in ("MAX_TRANSFER_INDEX_BYTES", "MAX_MANIFEST_BYTES", "MAX_INPUT_BYTES", "MAX_PACKET_BYTES"):
            with self.subTest(budget=constant), mock.patch.object(gate, constant, 1):
                self.refused().assert_not_called()
                self.assertFalse(self.work.exists())
        with mock.patch.object(gate, "MAX_TRACE_BYTES", 1):
            self.refused().assert_called_once()
        self.assertFalse(self.report.exists())

    def test_compatibility_index_loader_uses_bounded_reads(self):
        index = self.corpus / "TRANSFER_SHA256.tsv"
        original_open = Path.open
        probes = []
        @contextlib.contextmanager
        def controlled(path, *args, **kwargs):
            with original_open(path, *args, **kwargs) as stream:
                if path == index:
                    probe = ReadProbe(stream, chunk=7)
                    probes.append(probe)
                    yield probe
                else:
                    yield stream
        with mock.patch.object(Path, "open", controlled):
            records = gate.load_transfer_index(self.corpus)
        self.assertEqual(len(records), 5)
        self.assertEqual(probes[0].returned, index.stat().st_size)
        self.assertGreater(len(probes[0].requests), 2)

    def test_compatibility_index_growth_after_fstat_is_bounded(self):
        index = self.corpus / "TRANSFER_SHA256.tsv"
        original_open = Path.open
        limit = index.stat().st_size
        probes = []
        def grow():
            with original_open(index, "ab") as writer:
                writer.write(b"extra" * 20)
        @contextlib.contextmanager
        def controlled(path, *args, **kwargs):
            with original_open(path, *args, **kwargs) as stream:
                probe = ReadProbe(stream, before_first=grow)
                probes.append(probe)
                yield probe
        with mock.patch.object(gate, "MAX_TRANSFER_INDEX_BYTES", limit), \
                mock.patch.object(Path, "open", controlled):
            with self.assertRaisesRegex(ValueError, "grew beyond"):
                gate.load_transfer_index(self.corpus)
        self.assertEqual(probes[0].returned, limit + 1)

    def test_overlong_transfer_size_rejected_before_integer_conversion(self):
        data = b"artifact.bin\t" + b"9" * 10000 + b"\t" + b"0" * 64 + b"\n"
        with self.assertRaisesRegex(SystemExit, "invalid transfer size/hash"):
            gate.parse_transfer_index(data)


if __name__ == "__main__":
    unittest.main()
