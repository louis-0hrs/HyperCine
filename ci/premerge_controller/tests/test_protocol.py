# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import copy
import unittest
from ci.premerge_controller import check_protocol as c


class ReporterContract(unittest.TestCase):
    def setUp(self):
        self.sha='a'*40
        self.binding=c.external_id(self.sha,'b'*32,123)
        self.check={'id':1,'name':c.RELAY_CHECK,'head_sha':self.sha,'external_id':self.binding,
                    'app':{'id':987654},'status':'completed','conclusion':'success'}
        self.snapshot={'required_checks':[{'context':c.RELAY_CHECK,'app_id':987654}],
                       'checks':[self.check]}

    def evaluate(self):
        return c.evaluate(self.snapshot,c.RELAY_CHECK,987654,self.sha,self.binding)['accepted']

    def test_exact_app_sha_and_nonce_positive(self):
        self.assertTrue(self.evaluate())

    def test_same_name_user_status_is_not_a_check(self):
        self.snapshot['checks']=[]
        self.snapshot['statuses']=[{'context':c.RELAY_CHECK,'state':'success','sha':self.sha}]
        self.assertFalse(self.evaluate())

    def test_wrong_app_cannot_forge_accepted_success(self):
        self.check['app']['id']=15368
        self.assertFalse(self.evaluate())

    def test_stale_sha_never_greens_new_source(self):
        self.check['head_sha']='c'*40
        self.assertFalse(self.evaluate())

    def test_wrong_nonce_cannot_satisfy_new_attempt(self):
        self.check['external_id']=c.external_id(self.sha,'c'*32,123)
        self.assertFalse(self.evaluate())

    def test_missing_or_any_source_rule_does_not_qualify(self):
        for app_id in [None,-1,15368]:
            self.snapshot['required_checks'][0]['app_id']=app_id
            self.assertFalse(self.evaluate())

    def test_skipped_neutral_cancelled_failed_are_not_success(self):
        for value in ['skipped','neutral','cancelled','failure','timed_out',None]:
            self.check['conclusion']=value
            self.assertFalse(self.evaluate(),value)

    def test_controller_absence_or_pending_blocks(self):
        for state in ['queued','in_progress']:
            self.check['status']=state
            self.assertFalse(self.evaluate())
        self.snapshot['checks']=[]
        self.assertFalse(self.evaluate())

    def test_ambiguous_duplicate_is_not_accepted(self):
        self.snapshot['checks'].append(copy.deepcopy(self.check))
        self.assertFalse(self.evaluate())

    def test_app_identity_must_be_configured(self):
        self.assertFalse(c.evaluate(self.snapshot,c.RELAY_CHECK,None,self.sha,self.binding)['accepted'])

    def test_all_real_lanes_required_for_success(self):
        jobs=[{'name':lane,'run_id':123,'status':'completed','conclusion':'success'} for lane in c.LANES]
        self.assertEqual('success',c.lane_outcome(jobs,123,True))
        for value in ['cancelled','failure','skipped','neutral','timed_out',None]:
            jobs[-1]['conclusion']=value
            self.assertEqual('failure',c.lane_outcome(jobs,123,True))

    def test_missing_lane_pending_until_run_completes_then_failure(self):
        self.assertEqual('pending',c.lane_outcome([],123))
        self.assertEqual('failure',c.lane_outcome([],123,True))

    def test_wrong_run_or_duplicate_lane_fails_closed(self):
        jobs=[{'name':lane,'run_id':123,'status':'completed','conclusion':'success'} for lane in c.LANES]
        jobs[-1]['run_id']=456
        self.assertEqual('failure',c.lane_outcome(jobs,123,True))
        jobs[-1]['run_id']=123;jobs.append(copy.deepcopy(jobs[-1]))
        self.assertEqual('failure',c.lane_outcome(jobs,123,True))

    def test_corrupt_binding_rejected(self):
        for sha,nonce,run in [('main','b'*32,123),('a'*40,'nonce',123),('a'*40,'b'*32,True)]:
            with self.assertRaises(ValueError):c.external_id(sha,nonce,run)

    def test_current_required_inventory_has_no_implied_mac_gate(self):
        rules=[{'type':'required_status_checks','parameters':{'required_status_checks':[
            {'context':'Publication privacy (all new commits)','integration_id':15368}]}}]
        required=c.required_checks(rules)
        self.assertEqual([{'context':'Publication privacy (all new commits)','app_id':15368}],required)
        self.assertFalse(any(r['context'] in c.LEGACY_MAC_NAMES for r in required))
