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


REVIEWED=json.loads((Path(__file__).with_name('reviewed_step_changes.json')).read_text())


def original_steps_digest(workflow, job, current_steps):
    """Invert only exact reviewed steps before checking the frozen hash.

    Whole dictionaries come from reviewed #391/#394/#407, not current snapshots.
    New keys, conditions, environment/artifact changes and duplicates fail
    closed. Deep-copying preserves real workflow inputs for routing tests.
    """
    steps=copy.deepcopy(current_steps)
    # Undo later commits/steps first, preserving each historical step index.
    for change in reversed(REVIEWED['changes']):
        if (change['workflow'], change['job']) != (workflow, job):
            continue
        after=change['after']
        matches=[i for i, step in enumerate(steps)
                 if step.get('name') == after['name']]
        if (len(matches) != 1 or matches[0] != change['index_after']
                or steps[matches[0]] != after):
            raise ValueError((workflow, job, after['name'], 'unreviewed step'))
        index=matches[0]
        if change['before'] is None:
            del steps[index]
        else:
            steps[index]=copy.deepcopy(change['before'])
    replacement=BASE['reviewed_step_replacements_363'].get(workflow,{}).get(job)
    if replacement:
        matches=[step for step in steps if step.get('name')==replacement['name']]
        if len(matches) != 1 or matches[0].get('run') != replacement['run_after']:
            raise ValueError((workflow, job, 'unreviewed upstream verifier'))
        matches[0]['run']=replacement['run_before']
    return hashlib.sha256(json.dumps(steps,sort_keys=True).encode()).hexdigest()


def events():
    c=BASE['changed_paths']
    return [w.Event('push','refs/heads/main','main','branch',changed=c),
            w.Event('pull_request','refs/pull/42/merge','feature/x','branch',changed=c,pr_number=42),
            w.Event('pull_request','refs/pull/7/merge','contributor/x','branch',changed=c,pr_number=7,fork=True),
            w.Event('workflow_dispatch','refs/heads/main','main','branch',changed=c),
            w.Event('push','refs/heads/feature/x','feature/x','branch',changed=c)]


def expected_non_mac(event):
    # The frozen inventory predates #350's reviewed feature-push dedup.
    # Keep its full PR/main/manual assertions; only the redundant feature
    # builds and the optional Windows hook job are intentionally removed.
    rows=BASE['non_mac_jobs'][f'{event.name}:{event.ref}']
    if event.name == 'push' and event.ref != 'refs/heads/main':
        rows=[r for r in rows if (r[0],r[1]) in {
            ('ci-workflow-routing.yml','routing-contract'),
            ('privacy-guard.yml','publication-privacy')}]
    return rows


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
                    self.assertEqual(expected_non_mac(event),actual)
    def test_offline_has_no_hosted_mac_fallback_but_non_mac_jobs(self):
        for event in events():
            jobs=w.plan(self.docs,replace(event,vars={'MAC_MODE':'paused/host-unavailable'}))
            self.assertFalse(any(i.labels==['macos-15'] for i in jobs))
            before=expected_non_mac(event)
            for wf in ['ci-cpu.yml','ci-cuda-build.yml']:
                self.assertEqual(any(i[0]==wf for i in before),any(i.workflow==wf for i in jobs))
            if event.name != 'push' or event.ref == 'refs/heads/main':
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
        for wf, jobs in BASE['original_step_definitions_sha256'].items():
            for job, expected in jobs.items():
                with self.subTest(workflow=wf, job=job):
                    self.assertEqual(expected, original_steps_digest(
                        wf, job, self.docs[wf]['jobs'][job]['steps']))

    def test_privacy_still_runs_on_push_pr_with_same_jobs(self):
        for event in events():
            if event.name=='workflow_dispatch':continue
            jobs=[i for i in w.plan(self.docs,event) if i.workflow=='privacy-guard.yml']
            expected={'publication-privacy'}
            if event.name=='pull_request' or event.ref=='refs/heads/main':
                expected.add('windows-installed-hooks')
            self.assertEqual(expected,{i.job_id for i in jobs})
    def test_fallback_inputs_typed_and_off_by_default(self):
        for wf in ['ci-standalone.yml','quality-state.yml']:
            i=self.docs[wf]['on']['workflow_dispatch']['inputs']
            self.assertEqual('boolean',i['mac_hosted_fallback']['type']);self.assertIs(i['mac_hosted_fallback']['default'],False)
            self.assertEqual('string',i['mac_candidate_sha']['type']);self.assertEqual('',i['mac_candidate_sha']['default'])


class FrozenStepMutationTests(unittest.TestCase):
    """Causal RED controls: reviewed inverses must not hide new workflow drift."""
    def setUp(self):
        self.docs=w.load_workflows(str(ROOT/'.github/workflows'))

    def jobs(self):
        for wf, jobs in BASE['original_step_definitions_sha256'].items():
            for job in jobs:
                yield wf, job, self.docs[wf]['jobs'][job]['steps']

    def assertRejected(self, wf, job, steps):
        try:
            digest=original_steps_digest(wf, job, steps)
        except ValueError:
            return
        self.assertNotEqual(BASE['original_step_definitions_sha256'][wf][job], digest)

    def test_green_and_input_unchanged(self):
        original=copy.deepcopy(self.docs)
        for wf, job, steps in self.jobs():
            with self.subTest(workflow=wf, job=job):
                self.assertEqual(BASE['original_step_definitions_sha256'][wf][job],
                                 original_steps_digest(wf, job, steps))
        self.assertEqual(original, self.docs)

    def test_added_step_is_red_in_every_job(self):
        for wf, job, steps in self.jobs():
            for index in range(len(steps)+1):
                with self.subTest(workflow=wf, job=job, index=index):
                    changed=copy.deepcopy(steps)
                    changed.insert(index, {'name':'Unreviewed extra step','run':'echo extra'})
                    self.assertRejected(wf, job, changed)

    def test_removed_step_is_red_in_every_job(self):
        for wf, job, steps in self.jobs():
            for index in range(len(steps)):
                with self.subTest(workflow=wf, job=job, index=index):
                    changed=copy.deepcopy(steps)
                    del changed[index]
                    self.assertRejected(wf, job, changed)

    def test_reordered_step_is_red_in_every_job(self):
        for wf, job, steps in self.jobs():
            for index in range(len(steps)-1):
                with self.subTest(workflow=wf, job=job, index=index):
                    changed=copy.deepcopy(steps)
                    changed[index],changed[index+1]=changed[index+1],changed[index]
                    self.assertRejected(wf, job, changed)

    def test_changed_actual_command_is_red(self):
        for wf, job, steps in self.jobs():
            for index, step in enumerate(steps):
                if 'run' not in step:
                    continue
                with self.subTest(workflow=wf, job=job, index=index):
                    changed=copy.deepcopy(steps)
                    changed[index]['run']+=' # unreviewed command'
                    self.assertRejected(wf, job, changed)

    def test_artifact_retention_and_path_are_red(self):
        for wf, job, steps in self.jobs():
            for index, step in enumerate(steps):
                if step.get('uses') != 'actions/upload-artifact@v4':
                    continue
                for field, value in [('retention-days',8),('path','other-evidence.json')]:
                    with self.subTest(workflow=wf, job=job, index=index, field=field):
                        changed=copy.deepcopy(steps)
                        changed[index]['with'][field]=value
                        self.assertRejected(wf, job, changed)

    def test_extra_keys_on_reviewed_steps_are_red(self):
        for change in REVIEWED['changes']:
            wf,job=change['workflow'],change['job']
            steps=self.docs[wf]['jobs'][job]['steps']
            index=next(i for i,s in enumerate(steps) if s.get('name')==change['after']['name'])
            for field,value in [('continue-on-error',True),('if','false'),
                                ('env',{'UNREVIEWED':'1'}),('shell','unreviewed-shell')]:
                with self.subTest(pr=change['pr'],step=index,field=field):
                    changed=copy.deepcopy(steps)
                    changed[index][field]=value
                    self.assertRejected(wf, job, changed)

    def test_duplicate_reviewed_step_is_red(self):
        for change in REVIEWED['changes']:
            wf,job=change['workflow'],change['job']
            with self.subTest(pr=change['pr'],step=change['after']['name']):
                steps=copy.deepcopy(self.docs[wf]['jobs'][job]['steps'])
                steps.append(copy.deepcopy(change['after']))
                self.assertRejected(wf, job, steps)

    def test_unrelated_mac_step_change_is_red(self):
        steps=copy.deepcopy(self.docs['ci-standalone.yml']['jobs']['standalone']['steps'])
        matches=[step for step in steps if step.get('name')=='Install FFmpeg and Ninja (macOS)']
        self.assertEqual(1,len(matches))
        matches[0]['run']+=' # unreviewed Mac command'
        self.assertRejected('ci-standalone.yml','standalone',steps)

    def test_363_verifier_condition_is_still_guarded(self):
        for wf, job, steps in self.jobs():
            replacement=BASE['reviewed_step_replacements_363'].get(wf,{}).get(job)
            if not replacement:
                continue
            with self.subTest(workflow=wf,job=job):
                changed=copy.deepcopy(steps)
                step=next(s for s in changed if s.get('name')==replacement['name'])
                step['if']='false'
                self.assertRejected(wf, job, changed)
