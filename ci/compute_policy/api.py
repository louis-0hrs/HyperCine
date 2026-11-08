# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Read-only GitHub boundary for Mac admission. No workflow state mutation."""
import json
import re
import shutil
import subprocess
from .policy import COMPUTE, PREFIX, PRIVACY_ID


class GitHub:
    def __init__(self, *, armed=False, allow_enable=False, executable=None):
        self.armed = armed
        self.allow_enable = allow_enable
        self.executable = executable or shutil.which('gh')
        self.scopes = set()
        if not self.executable:
            raise RuntimeError('GitHub CLI unavailable')

    def workflow_write_verified(self):
        # Do not confuse accepted endpoint scopes with granted token scopes.
        # Unsupported/opaque fine-grained grants fail closed in this adapter.
        return {'repo', 'workflow'} <= self.scopes

    def __call__(self, endpoint, method='GET', body=None):
        fixed = {'user', PREFIX, PREFIX + '/rules/branches/main',
                 PREFIX + '/contents/.github/workflows/privacy-guard.yml?ref=main',
                 PREFIX + '/actions/workflows/ci-workflow-routing.yml'}
        fixed |= {f'{PREFIX}/actions/workflows/{n}' for n in [*COMPUTE, PRIVACY_ID]}
        ruleset = re.fullmatch(re.escape(PREFIX) + r'/rulesets/[1-9][0-9]{0,9}', endpoint)
        runs = re.fullmatch(re.escape(PREFIX) + r'/actions/runs\?status=(queued|in_progress|waiting|pending|requested)&per_page=100&page=([1-9]|10)', endpoint)
        write = re.fullmatch(re.escape(PREFIX) + r'/actions/workflows/(\d+)/(disable|enable)', endpoint)
        if method == 'GET':
            if endpoint not in fixed and not runs and not ruleset:
                raise ValueError('Read outside reviewed policy boundary')
        else:
            raise ValueError('Mac runtime is read-only; no GitHub mutation allowed')
        try:
            r = subprocess.run([self.executable, 'api', '--hostname', 'github.com', '--include', '--method', method, endpoint],
                               capture_output=True, timeout=45)
        except (OSError, subprocess.TimeoutExpired):
            raise RuntimeError('GitHub request unavailable; details suppressed') from None
        if r.returncode:
            status = re.search(rb'HTTP (\d{3})', r.stderr)
            code = status[1].decode() if status else 'unknown'
            raise RuntimeError('GitHub request failed (HTTP ' + code + '); details suppressed')
        try:
            headers, payload = r.stdout.replace(b'\r\n', b'\n').split(b'\n\n', 1)
            if endpoint == 'user':
                scopes = re.search(rb'(?im)^x-oauth-scopes:\s*([^\n]*)', headers)
                self.scopes = {x.strip() for x in scopes[1].decode().split(',')} if scopes else set()
            return json.loads(payload) if payload.strip() else None
        except (ValueError, UnicodeError):
            raise RuntimeError('GitHub response invalid; details suppressed') from None
