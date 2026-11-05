# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Barrier-ordered owner/worker races against the real canonical OS lock."""
import fcntl
import os
import threading
import unittest
from unittest.mock import Mock, patch
from ci.compute_policy import gate, policy as p
from ci.compute_policy.tests import test_policy


class AdmissionRaceTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_policy.PolicyTests();self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.transition('normal')
        self.api = self.fixture.api;self.path = self.fixture.state
        self.errors = []

    def worker(self, submit):
        return gate.admit_local(self.api, {'enabled': True}, self.path, self.fixture.probe, submit)

    def start(self, action):
        def run():
            try:action()
            except BaseException as exc:self.errors.append(exc)
        thread = threading.Thread(target=run, daemon=True);thread.start()
        self.addCleanup(thread.join, 4)
        return thread

    def finish(self, *threads):
        for thread in threads:
            thread.join(4);self.assertFalse(thread.is_alive(), 'Deadlocked owner/worker')
        if self.errors:raise self.errors[0]

    def test_pause_after_watch_snapshot_prevents_private_dispatch(self):
        snapshot_ready = threading.Event();pause_done = threading.Event()
        submit = Mock();result = [];watch = p.watch
        def stale_snapshot(*args, **kwargs):
            snapshot = watch(*args, **kwargs)
            snapshot_ready.set()
            if not pause_done.wait(3):raise AssertionError('Owner pause did not finish')
            return snapshot
        self.addCleanup(pause_done.set)
        with patch('ci.compute_policy.gate.p.watch', side_effect=stale_snapshot):
            worker = self.start(lambda: result.append(self.worker(submit)))
            self.assertTrue(snapshot_ready.wait(3))
            # Real authenticated owner command completes AFTER old watch and
            # BEFORE the worker is allowed to take its final admission lock.
            paused = self.fixture.transition()
            pause_done.set();self.finish(worker)
        self.assertEqual(p.PAUSED, paused['mode'])
        submit.assert_not_called();self.assertFalse(result[0]['allowed'])
        self.assertEqual(paused['generation'], self.fixture.load()['generation'])

    def test_dispatch_boundary_serializes_overlapping_owner_pause(self):
        in_submit = threading.Event();owner_probed = threading.Event();release = threading.Event()
        self.addCleanup(release.set)
        order = [];results = [];locked = []
        def submit(*, timeout):
            self.assertEqual(gate.SUBMISSION_TIMEOUT, timeout)
            in_submit.set()
            if not release.wait(3):raise AssertionError('Queue acknowledgment not released')
            order.append('queue-submitted')
        def owner():
            # Deterministically prove that the submission owns the actual same
            # flock, without scheduling guesses or sleep-based assertions.
            fd = os.open(self.path / 'compute-policy.lock', os.O_RDWR)
            try:
                try:fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:locked.append(True)
                else:locked.append(False);fcntl.flock(fd, fcntl.LOCK_UN)
            finally:os.close(fd);owner_probed.set()
            self.fixture.transition();order.append('pause-completed')
        worker = self.start(lambda: results.append(self.worker(submit)))
        self.assertTrue(in_submit.wait(3))
        pauser = self.start(owner)
        self.assertTrue(owner_probed.wait(3))
        release.set();self.finish(worker, pauser)
        self.assertEqual([True], locked, 'Owner lock absent during queue submission')
        self.assertEqual(['queue-submitted', 'pause-completed'], order)
        self.assertTrue(results[0]['allowed']);self.assertEqual(p.PAUSED, self.fixture.load()['mode'])
        # Work submitted before the pause may finish; no new submission after it.
        again = Mock();self.assertFalse(self.worker(again)['allowed']);again.assert_not_called()
        with p.journal(self.path):pass  # No lock held for the later VM lifetime.

    def test_final_admission_refreshes_health_session_and_live_inventory(self):
        watch = p.watch
        for change in ['health', 'inventory', 'security']:
            with self.subTest(change=change):
                self.fixture.transition('normal')
                submit = Mock();probe = self.fixture.probe
                def changed(*args, **kwargs):
                    state = watch(*args, **kwargs)
                    if change == 'health':self.fixture.probe = lambda: {}
                    elif change == 'inventory':self.api.rows[str(next(iter(p.COMPUTE)))]['state'] = 'active'
                    else:self.api.privacy_active = False
                    return state
                with patch('ci.compute_policy.gate.p.watch', side_effect=changed):
                    result = gate.admit_local(self.api, {'enabled': True}, self.path,
                                             lambda: self.fixture.probe(), submit)
                submit.assert_not_called();self.assertFalse(result['allowed'])
                self.fixture.probe = probe;self.api.privacy_active = True
                self.fixture.transition(reconcile=True)

    def test_queue_timeout_latches_pause_without_retry(self):
        submit = Mock(side_effect=TimeoutError('Uncertain queue acknowledgment'))
        result = self.worker(submit)
        submit.assert_called_once_with(timeout=gate.SUBMISSION_TIMEOUT)
        self.assertFalse(result['allowed']);self.assertEqual(p.PAUSED, self.fixture.load()['mode'])
        again = Mock();self.assertFalse(self.worker(again)['allowed']);again.assert_not_called()

    def test_old_zero_argument_or_vm_wait_callback_is_not_the_queue_interface(self):
        called = []
        def old_callback():called.append('would-wait-for-vm')
        self.assertFalse(self.worker(old_callback)['allowed']);self.assertEqual([], called)
        self.assertEqual(p.PAUSED, self.fixture.load()['mode'])
