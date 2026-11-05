# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Isolated policy mutations; only a designated regression failure kills one."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

MUTATIONS = [
 ('final-admission-state', 'gate.py', "            state = store.load()\n            live =", "            state = snapshot\n            live =", 'test_pause_after_watch_snapshot_prevents_private_dispatch'),
 ('dispatch-owner-lock', '../premerge_controller/state_store.py', '        fcntl.flock(lock_fd, fcntl.LOCK_EX)', '        pass  # Removed owner/admission serialization', 'test_dispatch_boundary_serializes_overlapping_owner_pause'),
 ('final-admission-check', 'gate.py', "if not p.admit(state, 'local', host_probe, generation=generation, live=live):", 'if False:', 'test_final_admission_refreshes_health_session_and_live_inventory'),
 ('watch-failure-generation', 'policy.py', "if state['mode'] != PAUSED or not state['attention']:", 'if False:', 'test_api_outage_invalidates_old_local_resume_plans'),
 ('owner-auth', 'policy.py', "if actor.get('id') != OWNER_ID or actor.get('login') != OWNER:", 'if False:', 'test_owner_identity_and_capability_required_before_mutation'),
 ('write-capability', 'policy.py', 'if not api.workflow_write_verified():', 'if False:', 'test_owner_identity_and_capability_required_before_mutation'),
 ('privacy-active', 'policy.py', "if privacy.get('id') != PRIVACY_ID or privacy.get('path') != '.github/workflows/privacy-guard.yml' or privacy.get('state') != 'active':", 'if False:', 'test_missing_privacy_or_unknown_required_gate_refuses_start'),
 ('unknown-required-gate', 'policy.py', "if any(c.get('context') not in SECURITY_CONTEXTS or SECURITY_CONTEXTS[c['context']] != c.get('integration_id') for c in checks):", 'if False:', 'test_missing_privacy_or_unknown_required_gate_refuses_start'),
 ('privacy-definition', 'policy.py', "if source.get('sha') != PRIVACY_BLOB:", 'if False:', 'test_changed_scanner_bypass_or_disabled_required_security_blocks_start'),
 ('ruleset-no-bypass', 'policy.py', "or row.get('bypass_actors') != []", 'or False', 'test_changed_scanner_bypass_or_disabled_required_security_blocks_start'),
 ('owner-plan-expiry', 'policy.py', "if not -5 <= time.time() - plan['created_at'] <= 300:", 'if False:', 'test_expired_owner_plan_never_changes_workflows'),
 ('quiet-plan', 'policy.py', "if quiet and facts['active_runs']:", 'if False:', 'test_busy_quiet_window_refuses_even_pause'),
 ('quiet-execute', 'policy.py', "if plan['quiet'] and facts['active_runs']:", 'if False:', 'test_new_run_between_plan_and_execute_refuses_start'),
 ('plan-generation', 'policy.py', "if generation != state['generation']:", 'if False:', 'test_stale_plan_creation_is_rejected'),
 ('execution-generation', 'policy.py', "if generation != state['generation'] or plan_id not in state['plans']:", 'if plan_id not in state["plans"]:', 'test_stale_plan_cannot_override_a_later_owner_decision'),
 ('inventory-drift', 'policy.py', "if facts['inventory'] != plan['inventory'] or facts['security_digest'] != plan['security_digest']:", 'if False:', 'test_external_state_change_is_not_blindly_restored'),
 ('original-disabled', 'policy.py', "if not new_hosted_grant and any(baseline[str(k)]['state'] != 'active' for k in selected):", 'if False:', 'test_originally_disabled_requires_deliberate_new_hosted_grant'),
 ('deployment-disabled', 'policy.py', "if enabled is not True:\n        raise RuntimeError('Deployment is disabled; no workflow mutation')", "if False:\n        raise RuntimeError('Deployment is disabled; no workflow mutation')", 'test_disabled_config_cannot_mutate_even_with_a_valid_plan'),
 ('durable-intent', 'policy.py', '    store.save(state)  # Durable intent precedes every remote mutation.', '    pass  # Deliberately missing durable intent.', 'test_every_remote_write_has_durable_matching_intent'),
 ('put-readback', 'policy.py', "if state['expected'][str(number)]['state'] != target:", 'if False:', 'test_failed_put_that_did_not_apply_cannot_be_called_success'),
 ('watch-owner-hosted', 'policy.py', "if state['mode'] == 'normal' and (not observed or observed['session_id'] != state['host_session']):", "if not observed:", 'test_hosted_choice_survives_mac_outage_without_automatic_changes'),
 ('health-session', 'policy.py', "or observed['session_id'] != state['host_session']", 'or False', 'test_new_health_session_requires_new_owner_local_resume'),
 ('admission-generation', 'policy.py', "if generation != state['generation'] or live is None or live.get('inventory') != state['expected'] or live.get('security_digest') != state['security_digest']:", "if live is None or live.get('inventory') != state['expected'] or live.get('security_digest') != state['security_digest']:", 'test_unverified_or_stale_admission_is_denied'),
 ('no-auto-resume', 'policy.py', "            event(state, 'watch_verified', mode=state['mode']);store.save(state)", "            if state['mode'] == PAUSED and healthy(host_probe):\n                api(f'{PREFIX}/actions/workflows/373401385/enable', 'PUT')\n            event(state, 'watch_verified', mode=state['mode']);store.save(state)", 'test_health_recovery_never_reenables_or_admits'),
 ('worker-no-passthrough', 'gate.py', "    if cfg.get('enabled') is not True:\n        return {'allowed': False, 'reason': 'policy-disabled'}", "    if cfg.get('enabled') is not True:\n        dispatch_private()\n        return {'allowed': True}", 'test_disabled_gate_never_invokes_api_or_private_dispatch'),
 ('old-worker-generation', 'gate.py', "if generation is not None and generation == state['generation']:", 'if generation is not None:', 'test_old_worker_error_cannot_overwrite_a_newer_owner_generation'),
 ('watch-transport-enable', 'api.py', "or (write[2] == 'enable' and not self.allow_enable)", 'or False', 'test_disarmed_or_watch_adapter_cannot_enable'),
 ('security-write-boundary', 'api.py', 'or int(write[1]) not in COMPUTE', 'or False', 'test_read_write_boundaries_never_touch_security_or_dispatch'),
]


def tests(root):
    return subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'ci/compute_policy/tests'],
                          cwd=root, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONPATH=str(root)),
                          capture_output=True, text=True, timeout=90)


def copy(root):
    for name in ['compute_policy', 'premerge_controller']:
        source = Path(__file__).parents[1] / name
        dest = root / 'ci' / name;dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, dest, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__);parser.add_argument('--output', type=Path)
    a = parser.parse_args(argv);results = []
    with tempfile.TemporaryDirectory(prefix='compute-policy-mutations-') as tmp:
        root = Path(tmp) / 'baseline';copy(root);baseline = tests(root)
        if baseline.returncode:raise RuntimeError('Policy baseline failed; receipt invalid')
        count = re.search(r'Ran (\d+) tests', baseline.stderr)
        for name, file, old, new, target in MUTATIONS:
            root = Path(tmp) / name;copy(root);path = root / 'ci/compute_policy' / file
            text = path.read_text()
            if old not in text:raise RuntimeError('Stale mutation marker: ' + name)
            text = text.replace(old, new, 1);compile(text, file, 'exec');path.write_text(text)
            run = tests(root)
            failures = re.findall(r'(?:FAIL|ERROR): (test_\w+)', run.stderr)
            killed = run.returncode != 0 and target in failures and 'Failed to import test module' not in run.stderr
            results.append({'id': name, 'module': file, 'designated_test': target, 'killed': killed})
            print(name + ': ' + ('KILLED' if killed else 'INVALID_OR_SURVIVED'), flush=True)
    receipt = {'baseline_tests': int(count[1]), 'mutations': len(results), 'killed': sum(x['killed'] for x in results),
               'all_killed': all(x['killed'] for x in results), 'results': results}
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True);a.output.write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k != 'results'}))
    if not receipt['all_killed']:raise SystemExit(1)


if __name__ == '__main__':main()
