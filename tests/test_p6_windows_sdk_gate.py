#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Portable falsifiers for the independently pinned Windows state gate."""
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import p6_windows_sdk_gate as gate


def witness():
    sessions = []
    for profile, limit in gate.LIMITS.items():
        sizes = [limit//120, limit//120, limit//240*9//10,
                 limit//240*9//10, limit//240*9//10, 100, 100]
        frames = []
        for i, size in enumerate(sizes):
            previous = sizes[i-1] if i else None
            frames.append(dict(frame_index=i, sdk_byte_length=size,
                               previous_sdk_sample_bytes=previous,
                               previous_bitrate_30fps=previous*240 if i else None,
                               sdk_lastgopbitcount_after_encode=size*8,
                               sdk_overbitrate_after_encode=gate.STATE[i],
                               sdk_rate_limiter_after_encode=0))
        sessions.append(dict(profile=profile, frames=frames,
                             limit_bits_per_second=limit, state_source=gate.STATE_SOURCE))
    return dict(schema=1, sdk_upstream_commit=gate.FROZEN, sessions=sessions)


class WindowsGateContract(unittest.TestCase):
    def test_positive_contract_only_not_packet_evidence(self):
        self.assertEqual(len(gate.validate_sessions(witness(), 'group-c-sdk-state')), 3)

    def test_missing_wrong_profile_order_and_state(self):
        for mode in ('profile', 'missing', 'reorder', 'truncated', 'state', 'rate', 'hold', 'source'):
            bad = witness(); frames = bad['sessions'][0]['frames']
            if mode == 'profile': bad['sessions'][0]['profile'] = 'high'
            if mode == 'missing': bad['sessions'].pop()
            if mode == 'reorder': frames[1], frames[2] = frames[2], frames[1]
            if mode == 'truncated': frames.pop()
            if mode == 'state': frames[3]['sdk_overbitrate_after_encode'] = 0
            if mode == 'rate': frames[3]['previous_bitrate_30fps'] += 1
            if mode == 'hold':
                frames[2]['sdk_byte_length'] = 100
                frames[2]['sdk_lastgopbitcount_after_encode'] = 800
                frames[3]['previous_sdk_sample_bytes'] = 100
                frames[3]['previous_bitrate_30fps'] = 24000
            if mode == 'source': bad['sessions'][0]['state_source'] = 'packet inferred'
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                gate.validate_sessions(bad, 'group-c-sdk-state')

    def test_sdk_state_requires_nonnegative_integer_fields(self):
        fields = ('sdk_byte_length', 'sdk_lastgopbitcount_after_encode',
                  'sdk_overbitrate_after_encode', 'sdk_rate_limiter_after_encode',
                  'previous_sdk_sample_bytes', 'previous_bitrate_30fps')
        for field in fields:
            for value in (False, True, 0.0, None, '0', -1):
                bad = witness()
                bad['sessions'][0]['frames'][1][field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gate.validate_sessions(bad, 'group-c-sdk-state')
        # Integral float aliases used to satisfy every numeric equality.
        for field in fields:
            bad = witness(); frame = bad['sessions'][0]['frames'][1]
            frame[field] = float(frame[field])
            with self.subTest(field=field), self.assertRaises(ValueError):
                gate.validate_sessions(bad, 'group-c-sdk-state')
        for value in (False, 0, 0.0):
            for field in ('previous_sdk_sample_bytes', 'previous_bitrate_30fps'):
                bad = witness(); bad['sessions'][0]['frames'][0][field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gate.validate_sessions(bad, 'group-c-sdk-state')
        bad = witness(); bad['sessions'][0]['limit_bits_per_second'] = float(gate.LIMITS['low'])
        with self.assertRaises(ValueError):
            gate.validate_sessions(bad, 'group-c-sdk-state')

    def test_zero_and_one_sdk_boolean_aliases_rejected(self):
        for field, index in (('sdk_overbitrate_after_encode', 0),
                             ('sdk_overbitrate_after_encode', 4),
                             ('sdk_rate_limiter_after_encode', 0)):
            bad = witness(); frame = bad['sessions'][0]['frames'][index]
            frame[field] = bool(frame[field])
            with self.subTest(field=field, index=index), self.assertRaises(ValueError):
                gate.validate_sessions(bad, 'group-c-sdk-state')

    def test_integer_state_field_shapes_and_values(self):
        fields = ('fs_overbitrate', 'fs_rate_limiter', 'previous_sample_bytes')
        gate.integer_state_fields(dict(zip(fields, (0, 1, 100))), fields)
        for bad in (None, [], False, 0, ''):
            with self.subTest(record=bad), self.assertRaises(ValueError):
                gate.integer_state_fields(bad, fields)
        for field in fields:
            for value in (False, True, 0.0, 1.0, float('nan'), float('inf'), None, '0', -1, [], {}):
                record = dict(zip(fields, (0, 1, 100))); record[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gate.integer_state_fields(record, fields)
            record = dict(zip(fields, (0, 1, 100))); del record[field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                gate.integer_state_fields(record, fields)

    def test_index_pin_fails_before_artifact_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root/'TRANSFER_SHA256.tsv').write_text('rewritten')
            with self.assertRaisesRegex(ValueError, 'pin mismatch'):
                gate.inventory(root, gate.PINS['group-c-sdk-state'])


if __name__ == '__main__':
    unittest.main()
