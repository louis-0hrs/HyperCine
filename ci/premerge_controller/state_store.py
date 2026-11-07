# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Owner-private approval storage. The service UID and root are trusted.

All operations are relative to a validated, pinned directory descriptor. No
candidate checkout, other local UID or shared writable directory is trusted.
"""
import contextlib
import fcntl
import json
import math
import os
from pathlib import Path
import re
import stat
import uuid

LEDGER = 'premerge.json'
LOCK = 'premerge.lock'


def _private_directory(info):
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid()
            or stat.S_IMODE(info.st_mode) != 0o700):
        raise RuntimeError('Approval state directory must be owned by the service UID with mode 0700')


def _regular_file(info):
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
            or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1):
        raise RuntimeError('Approval state files must be owned regular single-link files with mode 0600')


def _open_directory(path, create):
    path = Path(os.path.abspath(path))
    # Reject aliases instead of silently accepting a linked state directory.
    if path != path.resolve():
        raise RuntimeError('Approval state path must be canonical without symlinks')
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        parts = path.parts[1:]
        if not parts:
            raise RuntimeError('Root is not an approval state directory')
        for index, part in enumerate(parts):
            parent = os.fstat(fd)
            # A root-owned sticky temporary parent is safe against other UIDs
            # renaming our owned entry. Other writable ancestors are rejected.
            writable = parent.st_mode & 0o022
            sticky_root = parent.st_uid == 0 and parent.st_mode & stat.S_ISVTX
            if parent.st_uid not in {0, os.geteuid()} or (writable and not sticky_root):
                raise RuntimeError('Approval state ancestor is not trusted')
            if create and index == len(parts) - 1:
                try:
                    os.mkdir(part, 0o700, dir_fd=fd)
                except FileExistsError:
                    pass
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        _private_directory(os.fstat(fd))
        return fd
    except BaseException:
        os.close(fd)
        raise


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate ledger key')
        result[key] = value
    return result


def _validate_ledger(ledger):
    if not isinstance(ledger, dict):
        raise RuntimeError('Invalid approval ledger structure')
    states = {'approved', 'dispatching', 'queued', 'completed', 'invalidated', 'archived'}
    for key, record in ledger.items():
        if not isinstance(record, dict) or not isinstance(record.get('approval'), dict):
            raise RuntimeError('Invalid approval record')
        approval = record['approval']
        number = approval.get('pull_number')
        fields = ['head_sha', 'base_sha', 'tree_sha', 'test_merge_sha']
        if (type(number) is not int or number <= 0
                or any(not isinstance(approval.get(f), str) or not re.fullmatch('[0-9a-f]{40}', approval[f]) for f in fields)
                or not isinstance(record.get('relay_commit'), str) or not re.fullmatch('[0-9a-f]{40}', record['relay_commit'])
                or not isinstance(record.get('nonce'), str) or not re.fullmatch('[0-9a-f]{32}', record['nonce'])
                or not isinstance(record.get('state'), str) or record['state'] not in states
                or type(record.get('created')) not in {int, float} or not math.isfinite(record['created'])):
            raise RuntimeError('Invalid approval record fields')
        expected = f'{number}:{approval["head_sha"]}:{approval["base_sha"]}'
        if key not in {expected, expected + ':retired:' + record['nonce']}:
            raise RuntimeError('Approval ledger key does not match candidate')
    return ledger


class StateStore:
    def __init__(self, fd, validator=_validate_ledger, ledger=LEDGER, lock_name=LOCK, initial_factory=dict):
        if (ledger, lock_name) not in {(LEDGER, LOCK), ('compute-policy.json', 'compute-policy.lock')}:
            raise ValueError('Invalid state filenames')
        self.fd = fd
        self.validator = validator
        self.initial_factory = initial_factory
        self.ledger = ledger
        self.lock_name = lock_name

    def _check(self, name):
        try:
            info = os.stat(name, dir_fd=self.fd, follow_symlinks=False)
        except FileNotFoundError:
            return None
        _regular_file(info)
        return info

    def _check_all(self):
        _private_directory(os.fstat(self.fd))
        self._check(self.ledger)
        self._check(self.lock_name)
        # Never use the old predictable name, and refuse any leftover legacy
        # entry rather than interpreting it as recoverable approval data.
        try:
            os.stat(self.ledger + '.tmp', dir_fd=self.fd, follow_symlinks=False)
        except FileNotFoundError:
            return
        raise RuntimeError('Unexpected legacy approval temporary entry')

    def load(self):
        self._check_all()
        try:
            fd = os.open(self.ledger, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self.fd)
        except FileNotFoundError:
            return self.validator(self.initial_factory())
        try:
            _regular_file(os.fstat(fd))
            with os.fdopen(fd, 'r') as stream:
                fd = None
                value = json.load(stream, object_pairs_hook=_unique_object)
        except (ValueError, UnicodeError):
            raise RuntimeError('Invalid approval ledger JSON') from None
        finally:
            if fd is not None:
                os.close(fd)
        return self.validator(value)

    def save(self, value):
        self.validator(value)
        self._check_all()
        name = '.premerge-' + uuid.uuid4().hex + '.tmp'
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)
        try:
            _regular_file(os.fstat(fd))
            with os.fdopen(fd, 'w') as stream:
                fd = None
                json.dump(value, stream, indent=2, allow_nan=False)
                stream.write('\n')
                stream.flush()
                os.fsync(stream.fileno())
            self._check_all()
            os.replace(name, self.ledger, src_dir_fd=self.fd, dst_dir_fd=self.fd)
            os.fsync(self.fd)
        finally:
            if fd is not None:
                os.close(fd)
            try:
                os.unlink(name, dir_fd=self.fd)
            except FileNotFoundError:
                pass


@contextlib.contextmanager
def locked_state(path, create=False, *, validator=_validate_ledger, ledger=LEDGER, lock_name=LOCK, initial_factory=dict):
    """Validate storage before reading approvals or making privileged API calls."""
    fd = _open_directory(path, create)
    lock_fd = None
    try:
        store = StateStore(fd, validator, ledger, lock_name, initial_factory)
        store._check_all()
        lock_fd = os.open(store.lock_name, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=fd)
        _regular_file(os.fstat(lock_fd))
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        store._check_all()
        current = os.stat(store.lock_name, dir_fd=fd, follow_symlinks=False)
        opened = os.fstat(lock_fd)
        if (current.st_dev, current.st_ino) != (opened.st_dev, opened.st_ino):
            raise RuntimeError('Approval lock changed during acquisition')
        yield store
    finally:
        if lock_fd is not None:
            os.close(lock_fd)
        os.close(fd)
