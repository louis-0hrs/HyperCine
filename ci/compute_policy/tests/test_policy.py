# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import copy
import json
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from ci.compute_policy import policy as p


class FakeAPI:
    def __init__(self):
        self.rows = {str(n): {'id': n, 'path': path, 'state': 'active', 'updated_at': '0'} for n, path in p.COMPUTE.items()}
        self.calls = [];self.puts = [];self.version = 0;self.actor = p.OWNER_ID;self.admin = True;self.capability = True
        self.privacy_active = True;self.privacy_app = 15368;self.unknown_check = False;self.pr_required = True
        self.privacy_blob = p.PRIVACY_BLOB;self.bypass = [];self.routing_required = False;self.routing_active = True
        self.runs = [];self.failures = {};self.after_failures = {};self.read_fail = False
        self.signal_at = None;self.intent_path = None;self.require_intent = False;self.no_apply = False
        self.hook = None;self.persistent_failures = set()

    def workflow_write_verified(self):
        return self.capability

    def __call__(self, endpoint, method='GET', body=None):
        self.calls.append((endpoint, method))
        if method == 'PUT':
            number = endpoint.split('/')[-2];target = 'active' if endpoint.endswith('/enable') else 'disabled_manually'
            self.puts.append((int(number), target));index = len(self.puts)
            if self.require_intent:
                value = json.loads(self.intent_path.read_text())
                if value['pending'] != {'workflow': int(number), 'target': target} or value['audit'][-1]['kind'] != 'write_intent':
                    raise AssertionError('Remote write without durable matching intent')
            if (int(number), target) in self.persistent_failures:
                raise RuntimeError('persistent injected failure')
            if self.signal_at == index:
                raise KeyboardInterrupt()
            if index in self.failures:
                raise RuntimeError(self.failures.pop(index))
            if not self.no_apply:
                self.version += 1;self.rows[number]['state'] = target;self.rows[number]['updated_at'] = str(self.version)
            if self.hook:self.hook(self, int(number), target)
            if index in self.after_failures:
                raise RuntimeError(self.after_failures.pop(index))
            return None
        if endpoint == 'user':return {'id': self.actor, 'login': p.OWNER}
        if endpoint == p.PREFIX:return {'id': p.REPOSITORY_ID, 'default_branch': 'main', 'permissions': {'admin': self.admin}}
        if endpoint == f'{p.PREFIX}/contents/.github/workflows/privacy-guard.yml?ref=main':return {'sha': self.privacy_blob}
        if endpoint == f'{p.PREFIX}/rulesets/7':return {'id': 7, 'enforcement': 'active', 'bypass_actors': self.bypass}
        if endpoint == f'{p.PREFIX}/actions/workflows/ci-workflow-routing.yml':return {'path': '.github/workflows/ci-workflow-routing.yml', 'state': 'active' if self.routing_active else 'disabled_manually'}
        if endpoint == f'{p.PREFIX}/rules/branches/main':
            checks = [{'context': 'Publication privacy (all new commits)', 'integration_id': self.privacy_app}]
            if self.routing_required:checks.append({'context': 'Workflow routing contract (static)', 'integration_id': 15368})
            if self.unknown_check:checks.append({'context': 'Unreviewed compute gate', 'integration_id': 15368})
            return ([{'type': 'pull_request', 'ruleset_id': 7}] if self.pr_required else []) + [{'type': 'required_status_checks', 'ruleset_id': 7, 'parameters': {'required_status_checks': checks}}]
        if '/actions/runs?' in endpoint:return {'workflow_runs': copy.deepcopy(self.runs)}
        number = endpoint.rsplit('/', 1)[1]
        if number == str(p.PRIVACY_ID):return {'id': p.PRIVACY_ID, 'path': '.github/workflows/privacy-guard.yml', 'state': 'active' if self.privacy_active else 'disabled_manually'}
        if self.read_fail:raise RuntimeError('API inventory unavailable')
        if number in self.rows:return copy.deepcopy(self.rows[number])
        raise AssertionError(endpoint)


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.state = Path(self.tmp.name).resolve()
        self.api = FakeAPI();self.api.intent_path = self.state / 'compute-policy.json'
        self.probe = lambda: {'healthy': True, 'observed_at': time.time(), 'session_id': 'a' * 32}

    def load(self):
        with p.journal(self.state) as store:return store.load()

    def plan(self, target=p.PAUSED, **kwargs):
        return p.make_plan(self.api, self.state, self.load()['generation'], target, **kwargs)

    def execute(self, plan, **kwargs):
        return p.execute(self.api, self.state, plan['plan_id'], plan['generation'], enabled=True, host_probe=self.probe, **kwargs)

    def transition(self, target=p.PAUSED, **kwargs):
        return self.execute(self.plan(target, **kwargs))

    def all_off(self):
        return all(r['state'] != 'active' for r in self.api.rows.values())

    def admit(self, state, destination, **kwargs):
        return p.admit(state, destination, self.probe, generation=state['generation'], live=p.inspect(self.api), **kwargs)

    def test_default_is_paused_without_any_api_write(self):
        state = self.load();self.assertEqual(p.PAUSED, state['mode']);self.assertTrue(state['attention'])
        self.assertFalse(p.admit(state, 'hosted'));self.assertFalse(self.api.puts)

    def test_pause_captures_exact_immutable_original_states(self):
        inactive = str(list(p.COMPUTE)[1]);self.api.rows[inactive]['state'] = 'disabled_inactivity'
        plan = self.plan();state = self.execute(plan)
        self.assertEqual(plan['inventory'], state['plans'][state['baseline']]['inventory'])
        self.assertEqual('disabled_inactivity', self.api.rows[inactive]['state']);self.assertEqual(3, len(self.api.puts))
        self.assertTrue(self.all_off());p.validate(state)

    def test_pause_never_writes_privacy_rules_statuses_or_dispatch(self):
        self.transition()
        expected = {f'{p.PREFIX}/actions/workflows/{n}/disable' for n in p.COMPUTE}
        self.assertEqual(expected, {ep for ep, method in self.api.calls if method == 'PUT'})

    def test_explicit_owner_hosted_resume_only_selected_originally_active(self):
        self.transition();chosen = list(p.COMPUTE)[:2]
        state = self.transition('owner-authorized-hosted', selected=chosen)
        self.assertEqual('owner-authorized-hosted', state['mode']);self.assertTrue(self.admit(state, 'hosted'))
        self.assertEqual(set(chosen), {int(n) for n, r in self.api.rows.items() if r['state'] == 'active'})
        self.assertFalse(self.admit(state, 'local'))

    def test_normal_is_local_only_with_public_compute_off(self):
        state = self.transition('normal')
        self.assertTrue(self.all_off());self.assertTrue(self.admit(state, 'local'));self.assertFalse(self.admit(state, 'hosted'))
        self.assertFalse(any(target == 'active' for _, target in self.api.puts))

    def test_recovering_is_not_a_resume(self):
        state = self.transition('recovering')
        self.assertTrue(self.all_off());self.assertFalse(self.admit(state, 'local'));self.assertFalse(self.admit(state, 'hosted'))

    def test_host_outage_pauses_and_invalidates_old_generation(self):
        normal = self.transition('normal')
        state = p.watch(self.api, self.state, enabled=True, host_probe=lambda: {})
        self.assertEqual(p.PAUSED, state['mode']);self.assertGreater(state['generation'], normal['generation'])
        self.assertTrue(state['attention']);self.assertTrue(self.all_off())

    def test_health_recovery_never_reenables_or_admits(self):
        self.transition('normal');p.watch(self.api, self.state, enabled=True, host_probe=lambda: {})
        puts = list(self.api.puts)
        state = p.watch(self.api, self.state, enabled=True, host_probe=self.probe)
        self.assertEqual(p.PAUSED, state['mode']);self.assertTrue(state['attention']);self.assertEqual(puts, self.api.puts)
        self.assertFalse(self.admit(state, 'local'));self.assertFalse(self.admit(state, 'hosted'))

    def test_hosted_choice_survives_mac_outage_without_automatic_changes(self):
        self.transition();self.transition('owner-authorized-hosted', selected=list(p.COMPUTE))
        puts = list(self.api.puts);state = p.watch(self.api, self.state, enabled=True, host_probe=lambda: {})
        self.assertEqual('owner-authorized-hosted', state['mode']);self.assertEqual(puts, self.api.puts)

    def test_owner_identity_and_capability_required_before_mutation(self):
        for field, value in [('actor', 99), ('admin', False), ('capability', False)]:
            with self.subTest(field=field):
                old = getattr(self.api, field);setattr(self.api, field, value)
                with self.assertRaises(RuntimeError):self.plan()
                self.assertFalse(self.api.puts);setattr(self.api, field, old)

    def test_missing_privacy_or_unknown_required_gate_refuses_start(self):
        for field, value in [('privacy_active', False), ('privacy_app', 999), ('unknown_check', True), ('pr_required', False)]:
            with self.subTest(field=field):
                old = getattr(self.api, field);setattr(self.api, field, value)
                with self.assertRaises(RuntimeError):self.plan()
                self.assertFalse(self.api.puts);setattr(self.api, field, old)

    def test_busy_quiet_window_refuses_even_pause(self):
        self.api.runs = [{'id': 7, 'status': 'in_progress'}]
        with self.assertRaises(RuntimeError):self.plan()
        self.assertFalse(self.api.puts)

    def test_busy_emergency_pause_is_explicit_and_cancels_nothing(self):
        self.api.runs = [{'id': 7, 'status': 'queued'}]
        state = self.transition(quiet=False)
        self.assertTrue(self.all_off());self.assertEqual(p.PAUSED, state['mode'])
        self.assertFalse(any('/cancel' in ep for ep, _ in self.api.calls))
        with self.assertRaises(ValueError):self.plan('normal', quiet=False)

    def test_new_run_between_plan_and_execute_refuses_start(self):
        plan = self.plan();self.api.runs = [{'id': 7}]
        with self.assertRaises(RuntimeError):self.execute(plan)
        self.assertFalse(self.api.puts)

    def test_failed_inventory_read_refuses_start(self):
        self.api.read_fail = True
        with self.assertRaises(RuntimeError):self.plan()
        self.assertFalse(self.api.puts)

    def test_stale_plan_cannot_override_a_later_owner_decision(self):
        old = self.plan();self.execute(old)
        self.transition('owner-authorized-hosted', selected=list(p.COMPUTE))
        # Simulate an ABA state change: workflow updated_at is not a CAS token.
        self.api.rows = copy.deepcopy(old['inventory'])
        puts = list(self.api.puts)
        with self.assertRaises(RuntimeError):self.execute(old)
        self.assertEqual(puts, self.api.puts)
        self.assertEqual('owner-authorized-hosted', self.load()['mode'])

    def test_external_state_change_is_not_blindly_restored(self):
        self.transition();plan = self.plan('owner-authorized-hosted', selected=list(p.COMPUTE))
        self.api.rows[str(next(iter(p.COMPUTE)))]['updated_at'] = 'external-owner'
        puts = list(self.api.puts)
        with self.assertRaises(RuntimeError):self.execute(plan)
        self.assertEqual(puts, self.api.puts)
        with self.assertRaises(RuntimeError):self.plan()

    def test_reconcile_requires_fresh_explicit_pause_then_new_owner_choice(self):
        self.transition();first = str(next(iter(p.COMPUTE)));self.api.rows[first]['state'] = 'active'
        with self.assertRaises(RuntimeError):p.watch(self.api, self.state, enabled=True, host_probe=self.probe)
        state = self.transition(reconcile=True)
        self.assertTrue(self.all_off());self.assertEqual(p.PAUSED, state['mode'])
        with self.assertRaises(RuntimeError):self.plan('owner-authorized-hosted', selected=list(p.COMPUTE))
        state = self.transition('owner-authorized-hosted', selected=list(p.COMPUTE), new_hosted_grant=True)
        self.assertTrue(self.admit(state, 'hosted'))

    def test_originally_disabled_requires_deliberate_new_hosted_grant(self):
        self.api.rows[str(next(iter(p.COMPUTE)))]['state'] = 'disabled_manually'
        self.transition()
        with self.assertRaises(RuntimeError):self.plan('owner-authorized-hosted', selected=list(p.COMPUTE))
        self.assertFalse(any(target == 'active' for _, target in self.api.puts))

    def test_hosted_resume_requires_explicit_workflow_selection(self):
        with self.assertRaises(ValueError):self.plan('owner-authorized-hosted')
        with self.assertRaises(ValueError):self.plan('normal', selected=[next(iter(p.COMPUTE))])
        with self.assertRaises(ValueError):self.plan(selected=[p.PRIVACY_ID])

    def test_disabled_config_cannot_mutate_even_with_a_valid_plan(self):
        plan = self.plan()
        with self.assertRaises(RuntimeError):p.execute(self.api, self.state, plan['plan_id'], 0, enabled=False)
        self.assertFalse(self.api.puts)
        self.assertEqual({'disabled': True}, p.watch(self.api, self.state, enabled=False))

    def test_stale_unhealthy_future_and_unavailable_health_refuse_local_resume(self):
        plan = self.plan('normal')
        probes = [lambda: {}, lambda: {'healthy': True, 'observed_at': time.time() - 61},
                  lambda: {'healthy': True, 'observed_at': time.time() + 20},
                  lambda: {'healthy': True, 'observed_at': float('nan')}]
        for probe in probes:
            with self.subTest(probe=probe):
                with self.assertRaises(RuntimeError):p.execute(self.api, self.state, plan['plan_id'], 0, enabled=True, host_probe=probe)
                self.assertFalse(self.api.puts)

    def test_unverified_or_stale_admission_is_denied(self):
        state = self.transition('normal');facts = p.inspect(self.api)
        self.assertFalse(p.admit(state, 'local', self.probe))
        self.assertFalse(p.admit(state, 'local', self.probe, generation=state['generation'] - 1, live=facts))
        self.assertFalse(p.admit(state, 'local', self.probe, generation=state['generation'], live=facts, now=state['verified_at'] + 61))
        facts['inventory'] = {}
        self.assertFalse(p.admit(state, 'local', self.probe, generation=state['generation'], live=facts))

    def test_normal_and_hosted_repeated_decisions_are_remotely_idempotent(self):
        self.transition('normal');puts = list(self.api.puts);self.transition('normal');self.assertEqual(puts, self.api.puts)
        self.transition('owner-authorized-hosted', selected=list(p.COMPUTE));puts = list(self.api.puts)
        self.transition('owner-authorized-hosted', selected=list(p.COMPUTE));self.assertEqual(puts, self.api.puts)

    def test_every_remote_write_has_durable_matching_intent(self):
        self.api.require_intent = True
        self.transition();self.transition('owner-authorized-hosted', selected=list(p.COMPUTE))

    def test_api_failure_before_each_disable_never_enables_and_remains_visible(self):
        for index in range(1, 5):
            with self.subTest(index=index):
                self.setUp();plan = self.plan();self.api.failures[index] = 'injected disable failure'
                with self.assertRaises(RuntimeError):self.execute(plan)
                state = self.load();self.assertEqual(p.PAUSED, state['mode']);self.assertTrue(state['attention'])
                self.assertTrue(self.all_off());self.assertFalse(any(t == 'active' for _, t in self.api.puts))

    def test_api_failure_after_each_applied_disable_is_reconciled_off(self):
        for index in range(1, 5):
            with self.subTest(index=index):
                self.setUp();plan = self.plan();self.api.after_failures[index] = 'lost reply after disable'
                with self.assertRaises(RuntimeError):self.execute(plan)
                self.assertTrue(self.all_off());self.assertTrue(self.load()['attention'])

    def test_api_failure_during_each_manual_enable_compensates_only_toward_off(self):
        for index in range(1, 5):
            with self.subTest(index=index):
                self.setUp();self.transition();plan = self.plan('owner-authorized-hosted', selected=list(p.COMPUTE))
                start = len(self.api.puts);self.api.after_failures[start + index] = 'lost enable reply'
                with self.assertRaises(RuntimeError):self.execute(plan)
                tail = self.api.puts[start + index:]
                self.assertTrue(all(t != 'active' for _, t in tail));self.assertTrue(self.all_off());self.assertTrue(self.load()['attention'])

    def test_readback_failure_after_write_keeps_uncertainty_and_denies_admission(self):
        plan = self.plan()
        self.api.hook = lambda api, *_: setattr(api, 'read_fail', True)
        with self.assertRaises(RuntimeError):self.execute(plan)
        state = self.load();self.assertTrue(state['pending']);self.assertTrue(state['attention'])
        self.assertFalse(p.admit(state, 'hosted'));self.assertFalse(p.admit(state, 'local'))
        self.api.hook = None;self.api.read_fail = False
        # Independent fresh readback, explicit owner reconciliation; no re-enable.
        state = self.transition(reconcile=True)
        self.assertTrue(self.all_off());self.assertIsNone(state['pending'])

    def test_failed_put_that_did_not_apply_cannot_be_called_success(self):
        self.api.no_apply = True;plan = self.plan()
        with self.assertRaises(RuntimeError):self.execute(plan)
        self.assertTrue(self.load()['attention']);self.assertEqual(p.PAUSED, self.load()['mode'])

    def test_signal_during_transition_never_restores_hosted(self):
        for index in range(1, 5):
            with self.subTest(index=index):
                self.setUp();plan = self.plan();self.api.signal_at = index
                with self.assertRaises(KeyboardInterrupt):self.execute(plan)
                self.assertTrue(self.all_off());self.assertTrue(self.load()['attention'])
                self.assertFalse(any(t == 'active' for _, t in self.api.puts))

    def test_tampered_snapshot_and_audit_refuse_before_remote_access(self):
        self.plan();path = self.state / 'compute-policy.json';original = path.read_text()
        for field in ['plans', 'audit']:
            value = json.loads(original)
            if field == 'plans':next(iter(value['plans'].values()))['target'] = 'normal'
            else:value['audit'][0]['kind'] = 'forged'
            path.write_text(json.dumps(value));self.api.calls.clear()
            with self.assertRaises(RuntimeError):p.watch(self.api, self.state, enabled=True)
            self.assertEqual([], self.api.calls)
        path.write_text(original)

    def test_no_auto_recovery_after_process_loss_pending_intent(self):
        self.transition();value = self.load();value['pending'] = {'workflow': next(iter(p.COMPUTE)), 'target': 'active'}
        with p.journal(self.state) as store:store.save(value)
        puts = list(self.api.puts)
        with self.assertRaises(RuntimeError):p.watch(self.api, self.state, enabled=True, host_probe=self.probe)
        self.assertEqual(puts, self.api.puts);self.assertTrue(self.load()['attention'])
        state = self.transition(reconcile=True);self.assertEqual(p.PAUSED, state['mode'])

    def test_stale_plan_creation_is_rejected(self):
        self.transition()
        with self.assertRaises(RuntimeError):p.make_plan(self.api, self.state, 0, p.PAUSED)

    def test_partial_compensation_failure_is_visible_and_manual_pause_recovers(self):
        self.transition();chosen = list(p.COMPUTE)
        plan = self.plan('owner-authorized-hosted', selected=chosen)
        self.api.after_failures[len(self.api.puts) + 2] = 'enable reply lost'
        self.api.persistent_failures.add((chosen[0], 'disabled_manually'))
        with self.assertRaises(RuntimeError):self.execute(plan)
        state = self.load();self.assertEqual(p.PAUSED, state['mode']);self.assertTrue(state['attention'])
        self.assertTrue(state['pending']);self.assertFalse(self.all_off())
        self.assertFalse(self.admit(state, 'hosted'));self.assertFalse(self.admit(state, 'local'))
        facts = p.inspect(self.api);self.assertTrue(any(v['state'] == 'active' for v in facts['inventory'].values()))
        puts = len(self.api.puts);self.api.persistent_failures.clear()
        self.transition(reconcile=True)
        self.assertTrue(self.all_off());self.assertTrue(all(t != 'active' for _, t in self.api.puts[puts:]))

    def test_store_write_failure_stops_before_remote_change(self):
        from ci.premerge_controller.state_store import StateStore
        plan = self.plan()
        with patch.object(StateStore, 'save', side_effect=OSError('simulated journal unavailable')):
            with self.assertRaises(OSError):self.execute(plan)
        self.assertFalse(self.api.puts)

    def test_new_required_context_mid_transition_is_observable_not_bypassed(self):
        plan = self.plan()
        self.api.hook = lambda api, *_: setattr(api, 'unknown_check', True)
        with self.assertRaises(RuntimeError):self.execute(plan)
        state = self.load();self.assertTrue(state['attention']);self.assertFalse(self.all_off())
        self.assertEqual(1, len(self.api.puts))

    def test_unreviewed_journal_filename_pairs_are_rejected(self):
        from ci.premerge_controller.state_store import locked_state
        for ledger, lock in [('premerge.json', 'compute-policy.lock'), ('../outside', 'premerge.lock')]:
            with self.assertRaises(ValueError):
                with locked_state(self.state, ledger=ledger, lock_name=lock):pass

    def test_empty_existing_compute_journal_is_corruption_not_initialization(self):
        self.plan();(self.state / 'compute-policy.json').write_text('{}');self.api.calls.clear()
        with self.assertRaises(RuntimeError):p.watch(self.api, self.state, enabled=True)
        self.assertFalse(self.api.calls)

    def test_new_health_session_requires_new_owner_local_resume(self):
        state = self.transition('normal');old_generation = state['generation']
        new_probe = lambda: {'healthy': True, 'observed_at': time.time(), 'session_id': 'b' * 32}
        state = p.watch(self.api, self.state, enabled=True, host_probe=new_probe)
        self.assertEqual(p.PAUSED, state['mode']);self.assertGreater(state['generation'], old_generation)
        state = p.watch(self.api, self.state, enabled=True, host_probe=new_probe)
        self.assertEqual(p.PAUSED, state['mode']);self.assertFalse(self.admit(state, 'local'))
        plan = self.plan('normal')
        state = p.execute(self.api, self.state, plan['plan_id'], plan['generation'], enabled=True, host_probe=new_probe)
        self.assertEqual('b' * 32, state['host_session'])
        self.assertTrue(p.admit(state, 'local', new_probe, generation=state['generation'], live=p.inspect(self.api)))

    def test_health_session_change_during_resume_is_latched_paused(self):
        self.api.hook = lambda *_: None
        observations = iter([{'healthy': True, 'observed_at': time.time(), 'session_id': 'a' * 32},
                             {'healthy': True, 'observed_at': time.time(), 'session_id': 'b' * 32}])
        plan = self.plan('normal')
        with self.assertRaises(RuntimeError):p.execute(self.api, self.state, plan['plan_id'], 0, enabled=True, host_probe=lambda: next(observations))
        self.assertEqual(p.PAUSED, self.load()['mode']);self.assertTrue(self.all_off())

    def test_health_without_trusted_session_is_denied(self):
        self.assertFalse(p.healthy(lambda: {'healthy': True, 'observed_at': time.time()}))
        self.assertFalse(p.healthy(lambda: {'healthy': True, 'observed_at': time.time(), 'session_id': 'invalid'}))

    def test_active_run_pagination_cannot_hide_unrelated_work(self):
        original = self.api.__call__
        class Truncated:
            def __call__(inner, endpoint, method='GET', body=None):
                if '/actions/runs?' in endpoint:return {'total_count': 2, 'workflow_runs': [{'id': 1}]}
                return original(endpoint, method, body)
        with self.assertRaises(RuntimeError):p.active_runs(Truncated())
        self.assertFalse(self.api.puts)

    def test_changed_scanner_bypass_or_disabled_required_security_blocks_start(self):
        for field, bad in [('privacy_blob', 'f' * 40), ('bypass', [{'actor_id': 99}])]:
            original = getattr(self.api, field);setattr(self.api, field, bad)
            with self.subTest(field=field), self.assertRaises(RuntimeError):self.plan()
            self.assertFalse(self.api.puts);setattr(self.api, field, original)
        self.api.routing_required = True;self.api.routing_active = False
        with self.assertRaises(RuntimeError):self.plan()
        self.assertFalse(self.api.puts)

    def test_expired_owner_plan_never_changes_workflows(self):
        plan = self.plan()
        with patch('ci.compute_policy.policy.time.time', return_value=plan['created_at'] + 301):
            with self.assertRaises(RuntimeError):self.execute(plan)
        self.assertFalse(self.api.puts)

    def test_api_outage_invalidates_old_local_resume_plans(self):
        self.transition('normal');old = self.plan('normal')
        self.api.read_fail = True
        with self.assertRaises(RuntimeError):p.watch(self.api, self.state, enabled=True, host_probe=self.probe)
        self.api.read_fail = False
        self.assertGreater(self.load()['generation'], old['generation'])
        with self.assertRaises(RuntimeError):self.execute(old)
        self.assertEqual(p.PAUSED, self.load()['mode'])

    def test_owner_auth_outage_also_invalidates_old_resume_plans(self):
        self.transition('normal');old = self.plan('normal')
        with patch('ci.compute_policy.policy.authorize', side_effect=RuntimeError('Owner endpoint unavailable')):
            with self.assertRaises(RuntimeError):p.watch(self.api, self.state, enabled=True, host_probe=self.probe)
        self.assertGreater(self.load()['generation'], old['generation'])
        with self.assertRaises(RuntimeError):self.execute(old)
        self.assertEqual(p.PAUSED, self.load()['mode'])

    def test_state_directory_and_symlink_guards_apply_to_compute_journal(self):
        self.state.chmod(0o777);self.api.calls.clear()
        with self.assertRaises(RuntimeError):self.plan()
        self.assertEqual([], self.api.calls)
        self.state.chmod(0o700);self.plan();path = self.state / 'compute-policy.json';path.unlink();path.symlink_to(self.state / 'missing')
        self.api.calls.clear()
        with self.assertRaises(RuntimeError):p.watch(self.api, self.state, enabled=True)
        self.assertEqual([], self.api.calls)
