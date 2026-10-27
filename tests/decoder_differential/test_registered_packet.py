# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Synthetic identity-gate tests; no decoder result is represented here."""
import unittest
import contextlib
import io
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

import registered_packet as registered


class IdentityGateTest(unittest.TestCase):
    def test_source_gate_after_valid_packet(self):
        packet, source = b'packet', b'source'
        with patch.multiple(registered, PACKET_BYTES=len(packet),
                            PACKET_SHA256=registered.digest(packet),
                            SOURCE_BYTES=len(source), SOURCE_SHA256=registered.digest(source)):
            registered.verify_inputs(packet, source)
            for invalid in (source[:-1], b'change'):
                with self.assertRaises(ValueError):
                    registered.verify_inputs(packet, invalid)

    def test_unpinned_tool_is_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            tool = Path(directory) / 'tool'
            tool.write_bytes(b'wrong binary')
            with patch.object(registered, 'run') as run:
                with self.assertRaises(registered.PinnedToolUnavailable):
                    registered.tool_identity(tool, ['-version'], '0'*64)
                run.assert_not_called()

    def test_matching_tool_pin_records_verified_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            tool = Path(directory) / 'tool'
            tool.write_bytes(b'synthetic pinned binary')
            expected = registered.digest(tool.read_bytes())
            with patch.object(registered, 'run', return_value='synthetic version\n') as run:
                identity = registered.tool_identity(tool, ['-version'], expected)
            self.assertEqual(identity['binary_sha256'], expected)
            self.assertEqual(identity['version'], 'synthetic version')
            run.assert_called_once_with([tool, '-version'])

    def test_missing_pinned_decoder_is_explicitly_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / 'absent'
            args = SimpleNamespace(ffmpeg=missing, ffprobe=missing)
            with self.assertRaises(registered.PinnedToolUnavailable):
                registered.verify_tools(args)

    def test_wrong_packet_length_fails(self):
        with self.assertRaises(ValueError):
            registered.verify_inputs(b'x' * (registered.PACKET_BYTES - 1),
                                     b'\0' * registered.SOURCE_BYTES)

    def test_cli_unavailable_blocks_before_audit_and_redacts_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            packet, source = root/'packet', root/'source'
            packet.write_bytes(b'packet'); source.write_bytes(b'source')
            argv = ['registered_packet.py', '--packet', str(packet), '--source', str(source),
                    '--sdk-dump', str(root/'sdk'), '--ffmpeg', str(root/'absent'),
                    '--ffprobe', str(root/'absent-probe'), '--work', str(root/'work'),
                    '--output', str(root/'report.json')]
            stdout, stderr = io.StringIO(), io.StringIO()
            with patch('sys.argv', argv), patch.object(registered, 'verify_inputs'), \
                    patch.object(registered, 'audit_case') as audit, \
                    contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                self.assertEqual(registered.main(), 2)
            audit.assert_not_called()
            report = registered.json.loads(stdout.getvalue())
            self.assertEqual(report['alternate_decoder']['status'], 'UNAVAILABLE')
            self.assertTrue(report['release_blocked'])
            self.assertNotIn(directory, stdout.getvalue()+stderr.getvalue())
            self.assertFalse((root/'work').exists())

    def test_wrong_packet_digest_fails_at_registered_length(self):
        with self.assertRaises(ValueError):
            registered.verify_inputs(b'\0' * registered.PACKET_BYTES,
                                     b'\0' * registered.SOURCE_BYTES)

    def test_wrong_source_length_fails(self):
        with self.assertRaises(ValueError):
            registered.verify_inputs(b'\0' * registered.PACKET_BYTES,
                                     b'\0' * (registered.SOURCE_BYTES - 1))

    def test_packet_digest_constant_is_lowercase_sha256(self):
        self.assertEqual(len(registered.PACKET_SHA256), 64)
        self.assertEqual(registered.PACKET_SHA256, registered.PACKET_SHA256.lower())
        self.assertEqual(len(registered.SOURCE_SHA256), 64)


if __name__ == '__main__':
    unittest.main()
