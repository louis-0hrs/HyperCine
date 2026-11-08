# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import base64
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from ci.premerge_controller import candidate_binding as ca
from ci.premerge_controller import checks_reporter as r


class AppClient(unittest.TestCase):
    def setUp(self):
        self.cfg={'repository':r.FIXTURE_REPOSITORY,'app_id':987654,'installation_id':123,'repository_id':456}
        self.check={'id':7,'name':r.RELAY_CHECK,'head_sha':'a'*40,'external_id':'fixture-binding','app':{'id':987654}}
        self.calls=[]
        def transport(endpoint,token,method='GET',body=None):
            self.calls.append((endpoint,token,method,body))
            if endpoint.endswith('/check-runs'):return copy.deepcopy(self.check)
            if endpoint.endswith('/check-runs/7'):return copy.deepcopy(self.check)
            raise AssertionError(endpoint)
        self.client=r.FixtureReporter(self.cfg,transport=transport)
        self.client.token='fixture-test-token';self.client.expires=10**12

    def test_production_repository_is_rejected(self):
        with self.assertRaises(ValueError):r.FixtureReporter(dict(self.cfg,repository='louis-0hrs/HyperCine'))

    def test_missing_app_identity_is_rejected(self):
        with self.assertRaises(ValueError):r.FixtureReporter(dict(self.cfg,app_id=None))

    def test_create_binds_app_sha_nonce_and_labels_synthetic(self):
        self.client.create('a'*40,'fixture-binding')
        body=self.calls[-1][-1]
        self.assertEqual('queued',body['status'])
        self.assertIn('not native Mac',body['output']['summary'])

    def test_wrong_app_result_is_never_accepted(self):
        self.check['app']['id']=15368
        with self.assertRaises(RuntimeError):self.client.create('a'*40,'fixture-binding')

    def test_external_identity_is_checked_before_update(self):
        self.check['external_id']='old-binding'
        with self.assertRaises(RuntimeError):self.client.update(7,'a'*40,'fixture-binding','success')
        self.assertFalse(any(call[2]=='PATCH' for call in self.calls))

    def test_neutral_skipped_and_cancelled_cannot_be_emitted_as_accepted(self):
        for outcome in ['neutral','skipped','cancelled']:
            with self.assertRaises(ValueError):self.client.update(7,'a'*40,'fixture-binding',outcome)

    def test_only_success_or_failure_terminal_conclusions(self):
        self.client.update(7,'a'*40,'fixture-binding','failure')
        self.assertEqual('failure',self.calls[-1][-1]['conclusion'])
        self.client.update(7,'a'*40,'fixture-binding','pending')
        self.assertNotIn('conclusion',self.calls[-1][-1])

    def test_api_scope_cannot_cross_repositories(self):
        with self.assertRaises(ValueError):self.client.api('/repos/louis-0hrs/HyperCine/check-runs')

    def test_real_rsa_jwt_has_bounded_claims_and_valid_signature(self):
        # Generated disposable test key, never an actual GitHub credential.
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);key=root/'key.pem';pub=root/'public.pem';sig=root/'sig';msg=root/'msg'
            subprocess.run(['/usr/bin/openssl','genrsa','-out',str(key),'2048'],check=True,capture_output=True)
            key.chmod(0o600)
            token=r.app_jwt(987654,key.read_bytes(),now=1000)
            header,payload,signature=token.split('.')
            decode=lambda s:base64.urlsafe_b64decode(s+'='*(-len(s)%4))
            self.assertEqual({'iat':940,'exp':1540,'iss':'987654'},json.loads(decode(payload)))
            msg.write_bytes((header+'.'+payload).encode());sig.write_bytes(decode(signature))
            subprocess.run(['/usr/bin/openssl','rsa','-in',str(key),'-pubout','-out',str(pub)],check=True,capture_output=True)
            out=subprocess.run(['/usr/bin/openssl','dgst','-sha256','-verify',str(pub),'-signature',str(sig),str(msg)],capture_output=True)
            self.assertEqual(0,out.returncode)


class CandidateBinding(unittest.TestCase):
    def setUp(self):
        self.pr={'state':'open','merged':False,'draft':False,'mergeable':True,
                 'base':{'sha':'b'*40,'ref':'main','repo':{'full_name':r.FIXTURE_REPOSITORY}},
                 'head':{'sha':'a'*40,'ref':'case/head','repo':{'full_name':r.FIXTURE_REPOSITORY}},'merge_commit_sha':'c'*40}
        self.operator='louis-0hrs';self.parents=['b'*40,'a'*40]
        def api(endpoint):
            if endpoint=='user':return {'login':self.operator}
            if '/pulls/' in endpoint:return copy.deepcopy(self.pr)
            if '/git/commits/' in endpoint:return {'parents':[{'sha':s} for s in self.parents]}
            raise AssertionError(endpoint)
        self.api=api

    def test_exact_merge_candidate_can_be_revalidated(self):
        approval=ca.capture(self.api,1,'louis-0hrs','a'*40,'b'*40)
        self.assertEqual('c'*40,approval['candidate_sha'])
        self.assertEqual(approval,ca.revalidate(self.api,approval))

    def test_wrong_approving_actor_is_rejected(self):
        self.operator='untrusted'
        with self.assertRaises(RuntimeError):ca.capture(self.api,1,'louis-0hrs','a'*40,'b'*40)

    def test_unknown_mergeability_is_unresolved_not_a_conflict(self):
        self.pr['mergeable']=None
        with self.assertRaisesRegex(RuntimeError,'not resolved'):
            ca.capture(self.api,1,'louis-0hrs','a'*40,'b'*40)
        self.pr['mergeable']=False
        with self.assertRaisesRegex(RuntimeError,'not mergeable'):
            ca.capture(self.api,1,'louis-0hrs','a'*40,'b'*40)

    def test_forged_merge_parents_are_rejected(self):
        self.parents=['b'*40,'d'*40]
        with self.assertRaises(RuntimeError):ca.capture(self.api,1,'louis-0hrs','a'*40,'b'*40)

    def test_head_or_base_change_invalidates_approval(self):
        original=copy.deepcopy(self.pr)
        for field,value in [('head','d'*40),('base','e'*40)]:
            self.pr=copy.deepcopy(original);self.parents=['b'*40,'a'*40]
            approval=ca.capture(self.api,1,'louis-0hrs','a'*40,'b'*40)
            self.pr[field]['sha']=value;self.pr['merge_commit_sha']='f'*40
            self.parents=[self.pr['base']['sha'],self.pr['head']['sha']]
            with self.assertRaises(RuntimeError):ca.revalidate(self.api,approval)

    def test_production_target_is_rejected(self):
        self.pr['base']['repo']['full_name']='louis-0hrs/HyperCine'
        with self.assertRaises(RuntimeError):ca.capture(self.api,1,'louis-0hrs','a'*40,'b'*40)


class StrictHeadCandidate(unittest.TestCase):
    def setUp(self):
        self.original=CandidateBinding();self.original.setUp()
        self.strict=True;self.enforce=True;self.behind=0;self.equal_tree=True;self.main_sha='b'*40
        def api(ep):
            if ep.endswith('/branches/main'):return {'protected':True,'commit':{'sha':self.main_sha}}
            if '/git/ref/heads/' in ep:return {'object':{'sha':self.original.pr['head']['sha']}}
            if ep.endswith('/branches/main/protection'):
                return {'required_status_checks':{'strict':self.strict},'enforce_admins':{'enabled':self.enforce}}
            if '/compare/' in ep:return {'merge_base_commit':{'sha':'b'*40},'behind_by':self.behind}
            value=self.original.api(ep)
            if '/git/commits/' in ep:
                value['tree']={'sha':('d'*40 if self.equal_tree or ep.endswith('a'*40) else 'e'*40)}
            return value
        self.api=api

    def test_head_is_qualified_only_with_equal_merge_tree_and_strict_base(self):
        a=ca.capture_head(self.api,1,'louis-0hrs','a'*40,'b'*40)
        self.assertEqual('a'*40,a['candidate_sha'])
        self.assertEqual('c'*40,a['test_merge_sha'])
        self.assertEqual(a,ca.revalidate(self.api,a))

    def test_non_strict_or_admin_bypass_profile_is_rejected(self):
        for field in ['strict','enforce']:
            setattr(self,field,False)
            with self.assertRaises(RuntimeError):ca.capture_head(self.api,1,'louis-0hrs','a'*40,'b'*40)
            setattr(self,field,True)

    def test_current_main_invalidates_approval_even_when_pr_base_is_cached(self):
        a=ca.capture_head(self.api,1,'louis-0hrs','a'*40,'b'*40)
        self.main_sha='f'*40
        with self.assertRaisesRegex(RuntimeError,'Protected target base changed'):
            ca.revalidate(self.api,a)

    def test_behind_base_or_different_merge_tree_is_rejected(self):
        self.behind=1
        with self.assertRaises(RuntimeError):ca.capture_head(self.api,1,'louis-0hrs','a'*40,'b'*40)
        self.behind=0;self.equal_tree=False
        with self.assertRaises(RuntimeError):ca.capture_head(self.api,1,'louis-0hrs','a'*40,'b'*40)

    def test_reader_cannot_access_any_other_app_key(self):
        with patch.object(r.subprocess,'run') as run:
            with self.assertRaises(ValueError):r.keychain_key(15368)
            run.assert_not_called()
