# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Opt-in CLI/secret-provider adapters; never import machine configuration."""
import hashlib,json,os,re,shutil,stat,subprocess
from pathlib import Path
from .controller import SOURCE,APP_ID

class GitHubCLI:
    def __init__(self,cfg,executable=None):
        self.queue=cfg.get('repository');self.workflow=cfg.get('workflow')
        self.executable=executable or shutil.which('gh')
        if not self.executable:raise RuntimeError('GitHub CLI is not available')
    def __call__(self,endpoint,method='GET',body=None):
        path=endpoint.split('?',1)[0]
        if (any(part in {'.','..'} for part in path.split('/'))
            or any(s in endpoint.lower() for s in ['%2e','%2f','://','\n','\r','#'])):
            raise ValueError('Invalid API endpoint')
        allowed=endpoint=='user' or endpoint==f'repos/{SOURCE}' or endpoint.startswith(f'repos/{SOURCE}/')
        if self.queue:
            allowed=allowed or endpoint==f'repos/{self.queue}' or endpoint.startswith(f'repos/{self.queue}/')
        if not allowed:raise ValueError('API request outside approved source/queue')
        if method!='GET':
            if (method!='POST' or not self.queue or not isinstance(self.workflow,str)
                or not re.fullmatch(r'[A-Za-z0-9_.-]+',self.workflow)
                or endpoint!=f'repos/{self.queue}/actions/workflows/{self.workflow}/dispatches'):
                raise ValueError('Only an approved private-workflow dispatch is writable')
        args=[self.executable,'api','--method',method,endpoint]
        if body is not None:args+=['--input','-']
        try:
            result=subprocess.run(args,input=json.dumps(body).encode() if body is not None else None,capture_output=True,timeout=45)
        except (OSError,subprocess.TimeoutExpired):raise RuntimeError('GitHub CLI invocation failed; details suppressed') from None
        if result.returncode:
            status=re.search(rb'HTTP (\d{3})',result.stderr);code=status.group(1).decode() if status else 'unknown'
            raise RuntimeError(f'GitHub API failed (HTTP {code}); details suppressed')
        try:return json.loads(result.stdout) if result.stdout.strip() else None
        except ValueError:raise RuntimeError('GitHub API response invalid; details suppressed') from None

class KeyReader:
    """Separately reviewed, owner-controlled executable returns PEM over stdout."""
    def __init__(self,program,expected_sha256):
        self.program=Path(program) if program else None;self.expected=expected_sha256
    def __call__(self,app_id):
        if app_id!=APP_ID:raise ValueError('Unapproved signing identity')
        if not self.program or not self.program.is_absolute() or self.program.is_symlink():
            raise RuntimeError('Absolute owner-controlled key reader is required')
        try:info=self.program.stat()
        except OSError:raise RuntimeError('Key reader metadata unavailable; details suppressed') from None
        if (info.st_uid!=os.getuid() or not stat.S_ISREG(info.st_mode) or info.st_mode&0o022 or not os.access(self.program,os.X_OK)):
            raise RuntimeError('Key reader ownership or permissions are unsafe')
        if not isinstance(self.expected,str) or not re.fullmatch('[0-9a-f]{64}',self.expected):
            raise RuntimeError('Reviewed key-reader digest is required')
        try:digest=hashlib.sha256(self.program.read_bytes()).hexdigest()
        except OSError:raise RuntimeError('Key reader unavailable; details suppressed') from None
        if digest!=self.expected:
            raise RuntimeError('Key reader executable changed')
        try:result=subprocess.run([str(self.program),str(app_id)],capture_output=True,timeout=20)
        except (OSError,subprocess.TimeoutExpired):raise RuntimeError('Key reader invocation failed; details suppressed') from None
        if result.returncode or not 100<len(result.stdout)<16000 or not result.stdout.startswith(b'-----BEGIN'):
            raise RuntimeError('Key reader unavailable; content suppressed')
        return result.stdout
