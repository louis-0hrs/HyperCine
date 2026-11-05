# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import json
import os
from pathlib import Path
import stat
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch
from ci.premerge_controller import admin, state_store as s
from ci.premerge_controller.tests import test_controller


class StateTrustTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_controller.PremergeTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.target = self.root.parent / (self.root.name + '-untouched')
        self.target.write_text('sentinel')
        self.addCleanup(self.target.unlink, missing_ok=True)
        self.target.chmod(0o600)

    def reject_both(self):
        for action in [self.fixture.approve, self.fixture.cycle]:
            with self.subTest(action=action.__name__):
                self.fixture.api.reset_mock()
                with self.assertRaises((RuntimeError, OSError)):
                    action()
                self.fixture.api.assert_not_called()
                self.assertEqual([], self.fixture.events)
                self.assertEqual('sentinel', self.target.read_text())

    def test_owner_private_state_supports_approval_and_cycle(self):
        first = self.fixture.approve()
        self.assertEqual(first, self.fixture.approve())
        self.fixture.cycle()
        self.assertEqual(['create', 'dispatch'], [e[0] for e in self.fixture.events])
        for name in [s.LEDGER, s.LOCK]:
            info = (self.root / name).stat()
            self.assertEqual(os.geteuid(), info.st_uid)
            self.assertEqual(0o600, stat.S_IMODE(info.st_mode))
        self.assertFalse(list(self.root.glob('*.tmp')))

    def test_owner_private_missing_leaf_created_without_shared_parents(self):
        self.fixture.root = self.root / 'new-state'
        self.fixture.approve()
        self.assertEqual(0o700, stat.S_IMODE(self.fixture.root.stat().st_mode))

    def test_group_writable_state_rejected_before_approve_and_cycle(self):
        self.root.chmod(0o770)
        self.reject_both()

    def test_world_writable_state_rejected_before_approve_and_cycle(self):
        self.root.chmod(0o777)
        self.reject_both()

    def test_group_readable_state_is_also_rejected(self):
        self.root.chmod(0o750)
        self.reject_both()

    def test_foreign_owner_directory_rejected(self):
        # Isolated owner simulation; tests do not require chown/root.
        with patch.object(s.os, 'geteuid', return_value=os.geteuid() + 1):
            self.reject_both()

    def test_shared_writable_ancestor_rejected(self):
        self.root.chmod(0o777)
        self.fixture.root = self.root / 'private-child'
        self.fixture.root.mkdir(mode=0o700)
        self.reject_both()

    def test_state_directory_symlink_rejected(self):
        alias = self.root / 'alias'
        alias.symlink_to(self.root, target_is_directory=True)
        self.fixture.root = alias
        self.reject_both()

    def test_symlink_ancestor_rejected(self):
        actual = self.root / 'actual'
        actual.mkdir(mode=0o700)
        (actual / 'state').mkdir(mode=0o700)
        alias = self.root / 'alias'
        alias.symlink_to(actual, target_is_directory=True)
        self.fixture.root = alias / 'state'
        self.reject_both()

    def test_ledger_symlink_rejected_without_reading_target(self):
        (self.root / s.LEDGER).symlink_to(self.target)
        self.reject_both()

    def test_dangling_ledger_symlink_rejected(self):
        (self.root / s.LEDGER).symlink_to(self.root / 'missing')
        self.reject_both()
        self.assertFalse((self.root / 'missing').exists())

    def test_lock_symlink_rejected_without_touching_target(self):
        (self.root / s.LOCK).symlink_to(self.target)
        self.reject_both()

    def test_legacy_tmp_symlink_rejected_without_touching_target(self):
        (self.root / (s.LEDGER + '.tmp')).symlink_to(self.target)
        self.reject_both()

    def test_legacy_regular_tmp_rejected_instead_of_recovered(self):
        (self.root / (s.LEDGER + '.tmp')).write_text('{}')
        self.reject_both()

    def test_nonregular_ledger_rejected(self):
        (self.root / s.LEDGER).mkdir(mode=0o700)
        self.reject_both()

    def test_fifo_ledger_rejected_without_blocking(self):
        os.mkfifo(self.root / s.LEDGER, mode=0o600)
        self.reject_both()

    def test_fifo_lock_rejected_without_blocking(self):
        os.mkfifo(self.root / s.LOCK, mode=0o600)
        self.reject_both()

    def test_hardlinked_ledger_rejected(self):
        os.link(self.target, self.root / s.LEDGER)
        self.reject_both()

    def test_unsafe_ledger_mode_rejected(self):
        path = self.root / s.LEDGER
        path.write_text('{}')
        path.chmod(0o660)
        self.reject_both()

    def test_unsafe_lock_mode_rejected(self):
        path = self.root / s.LOCK
        path.write_text('')
        path.chmod(0o644)
        self.reject_both()

    def test_foreign_owner_ledger_rejected(self):
        path = self.root / s.LEDGER
        path.write_text('{}')
        path.chmod(0o600)
        original = s.os.stat
        def foreign(name, *args, **kwargs):
            info = original(name, *args, **kwargs)
            if name == s.LEDGER:
                values = list(info)
                values[4] = os.geteuid() + 1
                return os.stat_result(values)
            return info
        with patch.object(s.os, 'stat', side_effect=foreign):
            self.reject_both()

    def test_tampered_or_malformed_ledger_rejected(self):
        path = self.root / s.LEDGER
        for payload in ['not json', '[]', '{"x":{}}', '{"x":0}', '{"x":{},"x":{}}']:
            with self.subTest(payload=payload):
                path.write_text(payload)
                path.chmod(0o600)
                self.reject_both()

    def test_tampered_record_candidate_binding_rejected(self):
        self.fixture.approve()
        path = self.root / s.LEDGER
        value = json.loads(path.read_text())
        record = next(iter(value.values()))
        record['approval']['head_sha'] = 'f' * 40
        path.write_text(json.dumps(value))
        self.reject_both()

    def test_exclusive_temp_collision_does_not_follow_symlink(self):
        self.fixture.approve()
        ledger = self.fixture.ledger()
        before = (self.root / s.LEDGER).read_bytes()
        collision = self.root / ('.premerge-' + 'f' * 32 + '.tmp')
        collision.symlink_to(self.target)
        with s.locked_state(self.root) as store:
            with patch.object(s.uuid, 'uuid4', return_value=SimpleNamespace(hex='f' * 32)):
                with self.assertRaises(FileExistsError):
                    store.save(ledger)
        self.assertTrue(collision.is_symlink())
        self.assertEqual(before, (self.root / s.LEDGER).read_bytes())
        self.assertEqual('sentinel', self.target.read_text())

    def test_exclusive_temp_collision_does_not_truncate_regular_file(self):
        self.fixture.approve()
        before = (self.root / s.LEDGER).read_bytes()
        collision = self.root / ('.premerge-' + 'f' * 32 + '.tmp')
        collision.write_text('existing temporary sentinel')
        collision.chmod(0o600)
        with s.locked_state(self.root) as store:
            with patch.object(s.uuid, 'uuid4', return_value=SimpleNamespace(hex='f' * 32)):
                with self.assertRaises(FileExistsError):
                    store.save(self.fixture.ledger())
        self.assertEqual('existing temporary sentinel', collision.read_text())
        self.assertEqual(before, (self.root / s.LEDGER).read_bytes())

    def test_failed_atomic_replace_preserves_old_ledger_and_cleans_temp(self):
        self.fixture.approve()
        before = (self.root / s.LEDGER).read_bytes()
        with s.locked_state(self.root) as store:
            with patch.object(s.os, 'replace', side_effect=OSError('injected replace failure')):
                with self.assertRaises(OSError):
                    store.save(self.fixture.ledger())
        self.assertEqual(before, (self.root / s.LEDGER).read_bytes())
        self.assertFalse(list(self.root.glob('*.tmp')))

    def test_save_fsyncs_file_before_replace_and_directory_after(self):
        events = []
        fsync, replace = s.os.fsync, s.os.replace
        def synced(fd):
            events.append('directory-sync' if stat.S_ISDIR(os.fstat(fd).st_mode) else 'file-sync')
            return fsync(fd)
        def replaced(*args, **kwargs):
            events.append('replace')
            return replace(*args, **kwargs)
        with patch.object(s.os, 'fsync', side_effect=synced), patch.object(s.os, 'replace', side_effect=replaced):
            self.fixture.approve()
        self.assertEqual(['file-sync', 'replace', 'directory-sync'], events)

    def test_approve_cli_does_not_repair_or_accept_unsafe_directory(self):
        self.root.chmod(0o777)
        config = self.root / 'config.json'
        config.write_text(json.dumps({'premerge': self.fixture.cfg}))
        with self.assertRaises(RuntimeError):
            admin.main(['--config', str(config), '--state', str(self.root), '--approve', '1',
                        '--head', self.fixture.head, '--base', self.fixture.base],
                       api_factory=Mock(return_value=self.fixture.api))
        self.fixture.api.assert_not_called()
        self.assertEqual(0o777, stat.S_IMODE(self.root.stat().st_mode))

    def test_cycle_cli_rejects_unsafe_state_before_key_provider(self):
        self.root.chmod(0o777)
        config = self.root / 'config.json'
        config.write_text(json.dumps({'premerge': self.fixture.cfg}))
        with patch('ci.premerge_controller.admin.KeyReader') as reader:
            with self.assertRaises(RuntimeError):
                admin.main(['--config', str(config), '--state', str(self.root), '--cycle', '--execute'],
                           api_factory=Mock(return_value=self.fixture.api))
            reader.return_value.assert_not_called()
        self.fixture.api.assert_not_called()
