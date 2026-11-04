# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Canonical-ref candidate binding; fixture and explicitly selected HyperCine."""
from .checks_reporter import FIXTURE_REPOSITORY
import re
from urllib.parse import quote


def capture(api, pull_number, operator, expected_head, expected_base):
    return _capture(api,pull_number,operator,expected_head,expected_base)


def _capture(api, pull_number, operator, expected_head, expected_base, resolved_base=None,resolved_head=None,repository=FIXTURE_REPOSITORY,allow_draft=False):
    if type(pull_number) is not int or pull_number <= 0 or operator != 'louis-0hrs':
        raise ValueError('Unapproved local operator or pull number')
    if api('user').get('login') != operator:
        raise RuntimeError('Local approving actor identity mismatch')
    pr = api(f'repos/{repository}/pulls/{pull_number}')
    if pr.get('state') != 'open' or pr.get('merged') or (pr.get('draft') and not allow_draft):
        raise RuntimeError('Candidate is not an open non-draft PR')
    if pr.get('base', {}).get('repo', {}).get('full_name') != repository:
        raise RuntimeError('Candidate target repository mismatch')
    # No public PR author/label/comment is an approval signal. Approval is an
    # explicit local operator invocation and the current authenticated actor.
    head = pr.get('head', {}).get('sha')
    if resolved_head is not None:head=resolved_head
    base = pr.get('base', {}).get('sha')
    if resolved_base is not None:
        if pr.get('base',{}).get('ref')!='main':
            raise RuntimeError('Candidate target is not protected main')
        base=resolved_base
    merge = pr.get('merge_commit_sha')
    if head != expected_head or base != expected_base:
        raise RuntimeError('Candidate no longer equals the explicitly approved head/base')
    if any(not re.fullmatch('[0-9a-f]{40}', value or '') for value in [head,base,merge]):
        raise RuntimeError('Candidate merge is not resolved')
    if pr.get('mergeable') is None:
        raise RuntimeError('Candidate mergeability is not resolved')
    if pr.get('mergeable') is not True:
        raise RuntimeError('Candidate is not mergeable')
    commit = api(f'repos/{repository}/git/commits/{merge}')
    if [p.get('sha') for p in commit.get('parents', [])] != [base,head]:
        if resolved_base is not None:
            raise RuntimeError('Candidate merge parents are not resolved for approved refs')
        raise RuntimeError('Test-merge parents do not match approved base/head')
    return {'repository': repository, 'pull_number': pull_number,
            'operator': operator, 'head_sha': head, 'base_sha': base,
            'candidate_sha': merge, 'selection': 'test-merge'}


def revalidate(api, approval):
    capture_method = capture_head if approval.get('selection') == 'head-strict-current-base' else capture
    current = capture_method(api, approval['pull_number'], approval['operator'], approval['head_sha'], approval['base_sha'])
    if current != approval:
        raise RuntimeError('Candidate approval invalidated by head/base/merge change')
    return current


def capture_head(api, pull_number, operator, expected_head, expected_base):
    return capture_repository_head(api,FIXTURE_REPOSITORY,pull_number,operator,expected_head,expected_base)


def capture_repository_head(api, repository, pull_number, operator, expected_head, expected_base,require_strict=True,allow_draft=False):
    if repository not in {FIXTURE_REPOSITORY,'louis-0hrs/HyperCine'}:
        raise ValueError('Unapproved candidate repository')
    current_base=api(f'repos/{repository}/branches/main')
    if current_base.get('protected') is not True:
        raise RuntimeError('Candidate target main is not protected')
    if current_base.get('commit',{}).get('sha')!=expected_base:
        raise RuntimeError('Protected target base changed independently of cached PR metadata')
    pr=api(f'repos/{repository}/pulls/{pull_number}')
    if pr.get('head',{}).get('repo',{}).get('full_name')!=repository or not pr['head'].get('ref'):
        raise RuntimeError('Candidate source is not an owned source branch')
    current_head=api(f'repos/{repository}/git/ref/heads/{quote(pr["head"]["ref"],safe="/")}')['object']['sha']
    if current_head!=expected_head:raise RuntimeError('Source branch changed independently of cached PR metadata')
    approval=_capture(api,pull_number,operator,expected_head,expected_base,resolved_base=expected_base,resolved_head=current_head,repository=repository,allow_draft=allow_draft)
    protection=api(f'repos/{repository}/branches/main/protection')
    if require_strict and (protection.get('required_status_checks',{}).get('strict') is not True or protection.get('enforce_admins',{}).get('enabled') is not True):
        raise RuntimeError('Head checks require strict current-base protection including administrators')
    comparison=api(f'repos/{repository}/compare/{expected_base}...{expected_head}')
    if comparison.get('merge_base_commit',{}).get('sha')!=expected_base or comparison.get('behind_by')!=0:
        raise RuntimeError('Approved head does not contain the current protected base')
    head=api(f'repos/{repository}/git/commits/{expected_head}')
    merge=api(f"repos/{repository}/git/commits/{approval['candidate_sha']}")
    if head.get('tree',{}).get('sha')!=merge.get('tree',{}).get('sha') or not head.get('tree',{}).get('sha'):
        raise RuntimeError('Head and test-merge trees differ')
    approval['test_merge_sha']=approval['candidate_sha']
    approval['candidate_sha']=expected_head
    approval['selection']='head-strict-current-base' if require_strict else 'head-current-base-advisory'
    approval['tree_sha']=head['tree']['sha']
    return approval
