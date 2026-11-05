# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Actual workflow event plans compared with frozen non-Mac parent definitions."""
import copy
import hashlib
import json
import unittest
from dataclasses import replace
from pathlib import Path
from ci import workflow_event_matrix as w

ROOT=Path(__file__).parents[3]
BASE=json.loads((Path(__file__).with_name('hosted_non_mac_baseline.json')).read_text())


def events():
    c=BASE['changed_paths']
    return [w.Event('push','refs/heads/main','main','branch',changed=c),
            w.Event('pull_request','refs/pull/42/merge','feature/x','branch',changed=c,pr_number=42),
            w.Event('pull_request','refs/pull/7/merge','contributor/x','branch',changed=c,pr_number=7,fork=True),
            w.Event('workflow_dispatch','refs/heads/main','main','branch',changed=c),
            w.Event('push','refs/heads/feature/x','feature/x','branch',changed=c)]


class MacScopeTests(unittest.TestCase):
    def setUp(self):self.docs=w.load_workflows(str(ROOT/'.github/workflows'))
    def test_non_mac_jobs_equal_before_in_all_modes_and_events(self):
        for event in events():
            for mode in ['normal','paused/host-unavailable','owner-authorized-hosted']:
                with self.subTest(event=event.name,ref=event.ref,mode=mode):
                    # Mode is local admission state, never a workflow-disable switch.
                    changed=replace(event,vars={'HYPERCINE_CI_LOCAL_MAC':'1','MAC_MODE':mode},
                        inputs={'mac_hosted_fallback':mode=='owner-authorized-hosted','mac_candidate_sha':'0'*40})
                    actual=[[i.workflow,i.job_id,i.matrix_name,i.labels] for i in w.plan(self.docs,changed) if i.labels != ['macos-15']]
                    self.assertEqual(BASE['non_mac_jobs'][f'{event.name}:{event.ref}'],actual)
    def test_offline_has_no_hosted_mac_fallback_but_non_mac_jobs(self):
        for event in events():
            jobs=w.plan(self.docs,replace(event,vars={'MAC_MODE':'paused/host-unavailable'}))
            self.assertFalse(any(i.labels==['macos-15'] for i in jobs))
            before=BASE['non_mac_jobs'][f'{event.name}:{event.ref}']
            for wf in ['ci-cpu.yml','ci-cuda-build.yml']:
                self.assertEqual(any(i[0]==wf for i in before),any(i.workflow==wf for i in jobs))
            self.assertTrue(any(i.labels==['windows-2022'] for i in jobs))
            self.assertTrue(any(i.labels==['ubuntu-24.04'] for i in jobs))
    def test_only_explicit_owner_manual_exact_sha_starts_three_hosted_mac_jobs(self):
        for event in events():
            event=replace(event,inputs={'mac_hosted_fallback':True,'mac_candidate_sha':'0'*40})
            jobs=w.plan(self.docs,event);mac=[i for i in jobs if i.labels==['macos-15']]
            self.assertEqual(3 if event.name=='workflow_dispatch' else 0,len(mac))
            if mac:self.assertEqual({'macOS arm64 Release','macOS arm64 ASan UBSan','macOS arm64'},{i.matrix_name for i in mac})
    def test_non_owner_rerun_missing_ack_and_stale_sha_cannot_start_mac(self):
        event=events()[3]
        approved=replace(event,inputs={'mac_hosted_fallback':True,'mac_candidate_sha':'0'*40})
        for denied in [replace(approved,actor_id='99'),replace(approved,triggering_actor='other'),
                replace(approved,inputs={}),replace(approved,inputs={'mac_hosted_fallback':False,'mac_candidate_sha':'0'*40}),
                replace(approved,inputs={'mac_hosted_fallback':True,'mac_candidate_sha':'1'*40})]:
            self.assertFalse(any(i.labels==['macos-15'] for i in w.plan(self.docs,denied)))
    def test_no_public_self_hosted_runner_in_any_mode(self):
        for event in events():
            self.assertFalse(any('self-hosted' in i.labels for i in w.plan(self.docs,event)))
    def test_native_job_steps_unchanged(self):
        for wf,jobs in BASE['original_step_definitions_sha256'].items():
            for job,expected in jobs.items():
                actual=hashlib.sha256(json.dumps(self.docs[wf]['jobs'][job]['steps'],sort_keys=True).encode()).hexdigest()
                self.assertEqual(expected,actual,(wf,job))
    def test_privacy_still_runs_on_push_pr_with_same_jobs(self):
        for event in events():
            if event.name=='workflow_dispatch':continue
            jobs=[i for i in w.plan(self.docs,event) if i.workflow=='privacy-guard.yml']
            self.assertEqual({'publication-privacy','windows-installed-hooks'},{i.job_id for i in jobs})
    def test_fallback_inputs_typed_and_off_by_default(self):
        for wf in ['ci-standalone.yml','quality-state.yml']:
            i=self.docs[wf]['on']['workflow_dispatch']['inputs']
            self.assertEqual('boolean',i['mac_hosted_fallback']['type']);self.assertIs(i['mac_hosted_fallback']['default'],False)
            self.assertEqual('string',i['mac_candidate_sha']['type']);self.assertEqual('',i['mac_candidate_sha']['default'])
