# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Scoped GitHub App Checks transport; fixture client and reviewed production subclass.

FixtureReporter remains fixture-only. This module never changes rules, merges,
or distributes credentials to guests. Production use requires its separate guard.
Keys and bearer tokens are never command-line arguments or log content.
"""
import base64
import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import time
import urllib.error
import urllib.request
from .check_protocol import RELAY_CHECK

FIXTURE_REPOSITORY = 'louis-0hrs/HyperCI-Check-Acceptance'


def b64(value):
    return base64.urlsafe_b64encode(value).rstrip(b'=').decode()


def app_jwt(app_id, private_key, now=None):
    if type(app_id) is not int or app_id <= 0:
        raise ValueError('Invalid App identity')
    now = int(time.time() if now is None else now)
    message = (b64(b'{"alg":"RS256","typ":"JWT"}') + '.' +
               b64(json.dumps({'iat': now-60, 'exp': now+540, 'iss': str(app_id)},
                              separators=(',', ':')).encode())).encode()
    read_fd, write_fd = os.pipe()
    try:
        os.write(write_fd, private_key)
        os.close(write_fd); write_fd = None
        result = subprocess.run(['/usr/bin/openssl', 'dgst', '-sha256', '-sign', f'/dev/fd/{read_fd}'],
                                input=message, capture_output=True, pass_fds=(read_fd,), timeout=15)
    finally:
        os.close(read_fd)
        if write_fd is not None:
            os.close(write_fd)
    if result.returncode:
        raise RuntimeError('App signing failed; key/error content suppressed')
    return message.decode() + '.' + b64(result.stdout)


def keychain_key(app_id):
    if app_id != 5612385:
        raise ValueError('Unapproved Reporter App identity')
    raise RuntimeError('Host key-reader adapter is not configured')


def request(endpoint, token, method='GET', body=None):
    if not endpoint.startswith('/') or '..' in endpoint or '?' in endpoint or '://' in endpoint or '%2e' in endpoint.lower() or '\n' in endpoint or '\r' in endpoint:
        raise ValueError('Invalid GitHub API endpoint')
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request('https://api.github.com' + endpoint, data=data, method=method,
                                 headers={'Authorization': 'Bearer ' + token,
                                          'Accept': 'application/vnd.github+json',
                                          'Content-Type': 'application/json',
                                          'X-GitHub-Api-Version': '2022-11-28',
                                          'User-Agent': 'HyperCI-Checks-Acceptance'})
    # Never follow a redirect with an authorization header to another endpoint.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None
    try:
        with urllib.request.build_opener(NoRedirect).open(req, timeout=30) as response:
            return json.loads(response.read())
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f'Reporter API HTTP {exc.code}; response suppressed') from None
    except (OSError, ValueError):
        raise RuntimeError('Reporter API transport/decoding failed; details suppressed') from None


class _AppReporter:
    repository = None
    synthetic = False

    def __init__(self, cfg, key_provider=keychain_key, transport=request):
        if not self.repository or cfg.get('repository') != self.repository:
            raise ValueError('This reporter is restricted to the isolated fixture repository')
        for field in ['app_id', 'installation_id', 'repository_id']:
            if type(cfg.get(field)) is not int or cfg[field] <= 0:
                raise ValueError('Unconfigured fixture App identity')
        self.cfg = dict(cfg)
        self.key_provider = key_provider
        self.transport = transport
        self.token = None
        self.expires = 0

    def access_token(self):
        if self.token and time.time() < self.expires - 60:
            return self.token
        cfg = self.cfg
        jwt = app_jwt(cfg['app_id'], self.key_provider(cfg['app_id']))
        identity = self.transport('/app', jwt)
        if identity.get('id') != cfg['app_id']:
            raise RuntimeError('Reporter App identity mismatch')
        installation = self.transport(f'/repos/{self.repository}/installation', jwt)
        if installation.get('id') != cfg['installation_id'] or installation.get('app_id') != cfg['app_id']:
            raise RuntimeError('Reporter installation identity mismatch')
        result = self.transport(f"/app/installations/{cfg['installation_id']}/access_tokens", jwt, 'POST',
                                {'repository_ids': [cfg['repository_id']], 'permissions': {'checks': 'write'}})
        permissions = result.get('permissions', {})
        if (permissions.get('checks') != 'write' or set(permissions) - {'checks','metadata'}
            or permissions.get('metadata') not in {None, 'read'}):
            raise RuntimeError('Reporter token permissions exceed the Checks-only contract')
        expiry = datetime.datetime.fromisoformat(result['expires_at'].replace('Z', '+00:00')).timestamp()
        if not result.get('token') or not time.time() + 60 < expiry <= time.time() + 3700:
            raise RuntimeError('Invalid reporter token lifetime')
        self.token, self.expires = result['token'], expiry
        return self.token

    def api(self, endpoint, method='GET', body=None):
        prefix = f'/repos/{self.repository}/'
        if not endpoint.startswith(prefix):
            raise ValueError('Reporter request outside configured repository')
        return self.transport(endpoint, self.access_token(), method, body)

    def validate(self, check, source_sha, binding):
        if (check.get('name') != RELAY_CHECK or check.get('head_sha') != source_sha
            or check.get('external_id') != binding or check.get('app', {}).get('id') != self.cfg['app_id']):
            raise RuntimeError('Check App/source/dispatch binding mismatch')

    def summary(self, source_sha, binding):
        return 'Identity/protection test only; not native Mac qualification.'

    def create(self, source_sha, binding):
        if not re.fullmatch('[0-9a-f]{40}', source_sha or '') or not binding:
            raise ValueError('Invalid fixture source binding')
        body = {'name': RELAY_CHECK, 'head_sha': source_sha, 'external_id': binding, 'status': 'queued',
                'output': {'title': ('Synthetic acceptance fixture: queued' if self.synthetic else 'Approved Mac candidate: queued'),
                           'summary': self.summary(source_sha,binding)}}
        check = self.api(f'/repos/{self.repository}/check-runs', 'POST', body)
        self.validate(check, source_sha, binding)
        return check

    def update(self, check_id, source_sha, binding, outcome):
        if type(check_id) is not int or check_id <= 0 or outcome not in {'pending','success','failure'}:
            raise ValueError('Invalid fixture check update')
        endpoint = f'/repos/{self.repository}/check-runs/{check_id}'
        self.validate(self.api(endpoint), source_sha, binding)
        body = {'status': 'in_progress' if outcome == 'pending' else 'completed',
                'output': {'title': ('Synthetic acceptance fixture: ' if self.synthetic else 'Approved Mac candidate: ') + outcome,
                           'summary': self.summary(source_sha,binding)}}
        if outcome != 'pending':
            body['conclusion'] = outcome
        result = self.api(endpoint, 'PATCH', body)
        self.validate(result, source_sha, binding)
        return result


class FixtureReporter(_AppReporter):
    repository = FIXTURE_REPOSITORY
    synthetic = True
