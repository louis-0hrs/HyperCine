# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import contextlib
import io
import json
import subprocess
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from ci.compute_policy import api, admin, gate, policy as p
from ci.compute_policy.tests import test_policy


class AdapterTests(unittest.TestCase):
    def client(self, **kwargs):return api.GitHub(executable='gh', **kwargs)

    def test_read_write_boundaries_never_touch_security_or_dispatch(self):
        client = self.client(armed=True, allow_enable=True)
        for endpoint, method in [(f'{p.PREFIX}/actions/workflows/{p.PRIVACY_ID}/disable', 'PUT'),
                (f'{p.PREFIX}/actions/workflows/1/enable', 'PUT'),
                (f'{p.PREFIX}/actions/workflows/373401385/dispatches', 'POST'),
                (f'{p.PREFIX}/rules/branches/main', 'PUT'),
                (f'{p.PREFIX}/check-runs', 'POST'),
                ('repos/other/project/actions/workflows/373401385/disable', 'PUT'),
                (f'{p.PREFIX}/actions/runs?status=queued&per_page=100&page=11', 'GET')]:
            with self.subTest(endpoint=endpoint), patch('ci.compute_policy.api.subprocess.run', return_value=subprocess.CompletedProcess([],0,b'HTTP/2 204 No Content\n\n',b'')) as run:
                with self.assertRaises(ValueError):client(endpoint, method)
                run.assert_not_called()

    def test_disarmed_or_watch_adapter_cannot_enable(self):
        for cfg in [{}, {'armed': True}, {'allow_enable': True}]:
            with patch('ci.compute_policy.api.subprocess.run') as run:
                with self.assertRaises(ValueError):self.client(**cfg)(f'{p.PREFIX}/actions/workflows/373401385/enable', 'PUT')
                run.assert_not_called()

    def test_granted_scope_header_is_verified_without_output(self):
        client = self.client()
        payload = b'HTTP/2.0 200 OK\r\nX-OAuth-Scopes: repo, workflow\r\n\r\n{"id":248477380}'
        with patch('ci.compute_policy.api.subprocess.run', return_value=subprocess.CompletedProcess([], 0, payload, b'')):
            self.assertEqual(p.OWNER_ID, client('user')['id'])
        self.assertTrue(client.workflow_write_verified())
        # Accepted scopes are not proof of an actual credential grant.
        payload = b'HTTP/2.0 200 OK\nX-Accepted-OAuth-Scopes: repo, workflow\n\n{}'
        with patch('ci.compute_policy.api.subprocess.run', return_value=subprocess.CompletedProcess([], 0, payload, b'')):
            client('user')
        self.assertFalse(client.workflow_write_verified())

    def test_opaque_authentication_fails_closed_for_write_permission(self):
        client = self.client()
        with patch('ci.compute_policy.api.subprocess.run', return_value=subprocess.CompletedProcess([], 0, b'HTTP/2 200\n\n{}', b'')):
            client('user')
        self.assertFalse(client.workflow_write_verified())

    def test_error_response_stdout_and_stderr_are_never_disclosed(self):
        with patch('ci.compute_policy.api.subprocess.run', return_value=subprocess.CompletedProcess([], 1, b'PRIVATE-DUMMY', b'PRIVATE-DUMMY HTTP 403')):
            with self.assertRaises(RuntimeError) as error:self.client()('user')
        self.assertNotIn('PRIVATE-DUMMY', str(error.exception))
        self.assertIn('403', str(error.exception))

    def test_authorized_empty_put_response_is_supported(self):
        client = self.client(armed=True, allow_enable=True)
        with patch('ci.compute_policy.api.subprocess.run', return_value=subprocess.CompletedProcess([], 0, b'HTTP/2 204 No Content\n\n', b'')):
            with self.assertRaises(ValueError):client(f'{p.PREFIX}/actions/workflows/373401385/enable', 'PUT')

    def test_malformed_and_timed_out_api_responses_fail_closed(self):
        with patch('ci.compute_policy.api.subprocess.run', return_value=subprocess.CompletedProcess([], 0, b'not a valid response', b'')):
            with self.assertRaises(RuntimeError):self.client()('user')
        with patch('ci.compute_policy.api.subprocess.run', side_effect=subprocess.TimeoutExpired([], 1)):
            with self.assertRaises(RuntimeError):self.client()('user')


class CLIGateTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_policy.PolicyTests();self.fixture.setUp();self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.state;self.api = self.fixture.api
        self.config = self.root / 'config.json';self.config.write_text('{"enabled":true,"host_status":null}');self.config.chmod(0o600)
        self.args = ['--config', str(self.config), '--state', str(self.root)]

    def cli(self, args):
        with contextlib.redirect_stdout(io.StringIO()):
            return admin.main(self.args + args, api_factory=Mock(return_value=self.api))

    def test_cli_execute_requires_explicit_execution_and_enabled_config(self):
        for contents, flags in [('{"enabled":true,"host_status":null}', []), ('{"enabled":false,"host_status":null}', ['--execute'])]:
            self.config.write_text(contents)
            factory = Mock()
            with self.assertRaises(RuntimeError):admin.main(self.args + ['execute', '--plan', 'x', '--generation', '0'] + flags, api_factory=factory)
            factory.assert_not_called()

    def test_cli_manual_hosted_resume_requires_separate_acknowledgment(self):
        self.fixture.transition();plan = self.fixture.plan('owner-authorized-hosted', fallback_sha='b'*40);puts = list(self.api.puts)
        with self.assertRaises(RuntimeError):self.cli(['execute', '--plan', plan['plan_id'], '--generation', '1', '--execute'])
        self.assertEqual(puts, self.api.puts)
        result = self.cli(['execute', '--plan', plan['plan_id'], '--generation', '1', '--execute', '--allow-hosted'])
        self.assertEqual('owner-authorized-hosted', result['mode'])

    def test_cli_manual_reconcile_works_after_uncertain_process_loss(self):
        self.fixture.transition('normal')
        self.api.rows[str(next(iter(p.COMPUTE)))]['updated_at']='external'
        plan=self.cli(['plan','--generation','1','--target',p.PAUSED,'--reconcile','--reason','manual-recovery'])
        state=self.cli(['execute','--generation','1','--plan',plan['plan_id'],'--execute'])
        self.assertEqual(p.PAUSED,state['mode']);self.assertTrue(self.fixture.all_on());self.assertFalse(self.api.puts)

    def test_configuration_and_health_inputs_must_be_owned_private_regular_files(self):
        self.config.chmod(0o644)
        with self.assertRaises(RuntimeError):self.cli(['inspect'])
        self.config.chmod(0o600);link = self.root / 'link';link.symlink_to(self.config)
        with self.assertRaises(OSError):admin.owned_json(link)
        health = self.root / 'health.json';health.write_text(json.dumps({'healthy': True, 'observed_at': time.time(), 'session_id': 'a' * 32}));health.chmod(0o600)
        self.assertTrue(p.healthy(lambda: admin.owned_json(health)))

    def test_disabled_gate_never_invokes_api_or_private_dispatch(self):
        api_mock = Mock();dispatch = Mock()
        self.assertFalse(gate.admit_local(api_mock, {'enabled': False}, self.root, self.fixture.probe, dispatch)['allowed'])
        api_mock.assert_not_called();dispatch.assert_not_called()

    def test_guarded_local_dispatch_never_falls_back_to_hosted(self):
        self.fixture.transition('normal');dispatch = Mock()
        result = gate.admit_local(self.api, {'enabled': True}, self.root, self.fixture.probe, dispatch)
        self.assertTrue(result['allowed']);dispatch.assert_called_once_with(timeout=gate.SUBMISSION_TIMEOUT)
        puts = list(self.api.puts)
        result = gate.admit_local(self.api, {'enabled': True}, self.root, lambda: {}, dispatch)
        self.assertFalse(result['allowed']);self.assertEqual(1, dispatch.call_count);self.assertEqual(puts, self.api.puts)
        # Healthy again alone cannot restart private work.
        result = gate.admit_local(self.api, {'enabled': True}, self.root, self.fixture.probe, dispatch)
        self.assertFalse(result['allowed']);self.assertEqual(1, dispatch.call_count)

    def test_private_relay_error_is_latched_paused(self):
        self.fixture.transition('normal');dispatch = Mock(side_effect=RuntimeError('private queue unavailable'))
        result = gate.admit_local(self.api, {'enabled': True}, self.root, self.fixture.probe, dispatch)
        self.assertFalse(result['allowed']);state = self.fixture.load()
        self.assertEqual(p.PAUSED, state['mode']);self.assertTrue(state['attention'])

    def test_old_worker_error_cannot_overwrite_a_newer_owner_generation(self):
        self.fixture.transition('normal')
        old = self.fixture.load()['generation']
        self.fixture.transition('owner-authorized-hosted', fallback_sha='b'*40)
        gate._pause_if_current(self.root, old)
        self.assertEqual('owner-authorized-hosted', self.fixture.load()['mode'])

    def test_signal_handler_is_an_interruption_not_automatic_restore(self):
        with self.assertRaises(InterruptedError):admin.interrupted(15, None)

    def test_example_configuration_is_disabled(self):
        data = json.loads((Path(__file__).parents[1] / 'config.example.json').read_text())
        self.assertEqual({'enabled': False, 'host_status': None}, data)
