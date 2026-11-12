#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Pinned, no-clobber filesystem operations for the P6 writer gate.

POSIX operations are relative to open directory descriptors. On Windows each
ancestor is held without FILE_SHARE_DELETE, and reparse points are rejected.
The gate never deletes a run directory, including an incomplete run. These
primitives protect against pathname substitution; they are not a sandbox for
an untrusted encoder process or another process with the same user's rights.
"""
import contextlib
import os
from pathlib import Path, PureWindowsPath
import stat
import uuid


def _validate_path_spelling(value, path_type=Path):
    """Validate spelling before abspath/Win32 can discard suffixes.

    Anchors are syntax, not filename components. UNC server/share names still
    need validation. Device namespaces and drive-relative paths are refused.
    path_type permits host-independent Windows lexical regression tests.
    """
    path = path_type(value)
    if isinstance(path, PureWindowsPath):
        drive = path.drive
        if drive.startswith(("\\\\?\\", "\\\\.\\")):
            raise ValueError("device path is not accepted")
        if drive.startswith("\\\\"):
            anchor_parts = drive[2:].split("\\")
            if len(anchor_parts) != 2:
                raise ValueError("invalid UNC anchor")
            for name in anchor_parts:
                component(name)
        elif drive:
            if (len(drive) != 2 or not drive[0].isascii() or
                    not drive[0].isalpha() or drive[1] != ":" or not path.root):
                raise ValueError("drive-relative or device path is not accepted")
        elif path.root:
            raise ValueError("drive-relative root is not accepted")
    for name in path.parts[1:] if path.anchor else path.parts:
        component(name)
    return path


def absolute_path(value):
    path = _validate_path_spelling(value)
    return Path(os.path.abspath(path))


def component(value):
    if (not isinstance(value, str) or not value or value in (".", "..") or
            any(c in value for c in ("/", "\\", ":", '<', '>', '"', '|', '?', '*')) or
            any(ord(c) < 32 for c in value) or
            value.endswith((".", " "))):
        raise ValueError("invalid path component")
    stem = value.split(".", 1)[0].rstrip(" ").upper()
    if stem in {"CON", "PRN", "AUX", "NUL"} or (
            len(stem) == 4 and stem[:3] in ("COM", "LPT") and stem[3] in "123456789¹²³"):
        raise ValueError("reserved path component")
    return value


def within(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def bounded_chunks(stream, maximum, exact=None):
    """Read at most the admitted extent plus one overflow-probe byte.

    The descriptor size is an early refusal only: a producer can append or
    truncate after fstat. Each actual read therefore has a remaining budget.
    Short reads are accumulated; only an empty read denotes EOF. An overflow
    probe is never yielded to a caller that copies bytes into owned output.
    """
    if type(maximum) is not int or maximum <= 0:
        raise ValueError("invalid file read limit")
    if exact is not None and (type(exact) is not int or not 0 <= exact <= maximum):
        raise ValueError("invalid exact file length")
    info = os.fstat(stream.fileno())
    if not stat.S_ISREG(info.st_mode):
        raise ValueError("expected a regular file")
    if not 0 <= info.st_size <= maximum or (info.st_size == 0 and exact != 0):
        raise ValueError("file exceeds read limit or is empty")
    if exact is not None and info.st_size != exact:
        raise ValueError("file byte length mismatch")
    extent = maximum if exact is None else exact
    consumed = 0
    while consumed < extent:
        block = stream.read(min(64 * 1024, extent - consumed))
        if not block:
            if consumed == 0 or (exact is not None and consumed != exact):
                raise ValueError("file ended before declared byte length")
            return
        consumed += len(block)
        yield block
    if stream.read(1):
        raise ValueError("file grew beyond admitted byte length")


def read_bounded(stream, maximum, exact=None):
    """Return one bounded byte string for both hashing and interpretation."""
    return b"".join(bounded_chunks(stream, maximum, exact))


if os.name == "nt":
    import ctypes
    from ctypes import wintypes
    import msvcrt

    class _Info(ctypes.Structure):
        _fields_ = [("attributes", wintypes.DWORD),
                    ("creation", wintypes.FILETIME),
                    ("access", wintypes.FILETIME),
                    ("write", wintypes.FILETIME),
                    ("volume", wintypes.DWORD),
                    ("size_high", wintypes.DWORD),
                    ("size_low", wintypes.DWORD),
                    ("links", wintypes.DWORD),
                    ("index_high", wintypes.DWORD),
                    ("index_low", wintypes.DWORD)]

    class _FileIdInfo(ctypes.Structure):
        _fields_ = [("volume", ctypes.c_ulonglong),
                    ("identifier", ctypes.c_ubyte * 16)]

    _kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                   wintypes.DWORD, wintypes.LPVOID,
                                   wintypes.DWORD, wintypes.DWORD,
                                   wintypes.HANDLE]
    _kernel.CreateFileW.restype = wintypes.HANDLE
    _kernel.GetFileInformationByHandle.argtypes = [wintypes.HANDLE,
                                                   ctypes.POINTER(_Info)]
    _kernel.GetFileInformationByHandle.restype = wintypes.BOOL
    _kernel.GetFileInformationByHandleEx.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                                     wintypes.LPVOID, wintypes.DWORD]
    _kernel.GetFileInformationByHandleEx.restype = wintypes.BOOL
    _kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel.CloseHandle.restype = wintypes.BOOL

    def _win_open(path, directory=False, create=False):
        access = 0x80 if directory else (0x40000000 if create else 0x80000000)
        # Deny deletion/renaming while this handle is alive. Opening the
        # reparse point itself allows rejecting junctions before traversal.
        handle = _kernel.CreateFileW(str(path), access, 0x1 | 0x2, None,
                                    1 if create else 3,
                                    0x00200000 | (0x02000000 if directory else 0),
                                    None)
        if handle == wintypes.HANDLE(-1).value:
            raise OSError(ctypes.get_last_error(), "cannot pin filesystem entry")
        info = _Info()
        if not _kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
            _kernel.CloseHandle(handle)
            raise OSError("cannot inspect pinned filesystem entry")
        if info.attributes & 0x400 or bool(info.attributes & 0x10) != directory:
            _kernel.CloseHandle(handle)
            raise ValueError("linked or unexpected filesystem entry")
        # FILE_ID_INFO is the documented identity tuple, including the full
        # 128-bit identifier needed by ReFS. A shortened 64-bit ID can alias
        # another object; unavailable identity support therefore fails closed.
        identity = _FileIdInfo()
        if not _kernel.GetFileInformationByHandleEx(
                handle, 18, ctypes.byref(identity), ctypes.sizeof(identity)):
            _kernel.CloseHandle(handle)
            raise OSError("filesystem does not provide full file identities")
        return handle, (identity.volume, bytes(identity.identifier))


class Directory:
    """Existing directory pinned without following any symlink component."""

    def __init__(self, path, fd=None, handles=None, identity=None, requested=None,
                 ancestors=()):
        self.path = path
        self.requested = requested or path
        self.fd = fd
        self.handles = handles or []
        self.identity = identity
        self.ancestors = ancestors

    @classmethod
    def open(cls, value):
        requested = absolute_path(value)
        # Canonicalize existing parent aliases (including macOS /var), then
        # anchor the complete canonical chain without following replacement
        # links. All subsequent I/O uses these handles, never the alias.
        path = requested.resolve(strict=True)
        if os.name == "nt":
            handles = []
            ancestors = []
            try:
                current = Path(path.anchor)
                handle, identity = _win_open(current, directory=True)
                handles.append(handle)
                ancestors.append(identity)
                for name in path.parts[1:]:
                    current /= component(name)
                    handle, identity = _win_open(current, directory=True)
                    handles.append(handle)
                    ancestors.append(identity)
                return cls(path, handles=handles, identity=identity, requested=requested,
                           ancestors=tuple(ancestors))
            except BaseException:
                for handle in reversed(handles):
                    _kernel.CloseHandle(handle)
                raise
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
        fd = os.open(path.anchor, flags)
        info = os.fstat(fd)
        ancestors = [(info.st_dev, info.st_ino)]
        try:
            for name in path.parts[1:]:
                child = os.open(component(name), flags, dir_fd=fd)
                os.close(fd)
                fd = child
                info = os.fstat(fd)
                ancestors.append((info.st_dev, info.st_ino))
            info = os.fstat(fd)
            return cls(path, fd=fd, identity=(info.st_dev, info.st_ino), requested=requested,
                       ancestors=tuple(ancestors))
        except BaseException:
            os.close(fd)
            raise

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        for handle in reversed(self.handles):
            _kernel.CloseHandle(handle)
        self.handles = []

    def visible(self):
        """Refuse ambiguous external-process paths after a directory rename."""
        with Directory.open(self.requested) as current:
            if current.identity != self.identity:
                raise ValueError("output directory identity changed")

    def names(self):
        return os.listdir(self.path if os.name == "nt" else self.fd)

    def info(self, name):
        component(name)
        if os.name == "nt":
            return os.lstat(self.path / name)
        return os.stat(name, dir_fd=self.fd, follow_symlinks=False)

    def require_absent(self, name):
        try:
            self.info(name)
        except FileNotFoundError:
            return
        raise ValueError("output destination already exists")

    def child(self, name, create=False):
        component(name)
        if os.name == "nt":
            if create:
                os.mkdir(self.path / name, 0o700)
            handle, identity = _win_open(self.path / name, directory=True)
            return Directory(self.path / name, handles=[handle], identity=identity,
                             requested=self.requested / name,
                             ancestors=self.ancestors + (identity,))
        if create:
            os.mkdir(name, 0o700, dir_fd=self.fd)
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
        fd = os.open(name, flags, dir_fd=self.fd)
        info = os.fstat(fd)
        return Directory(self.path / name, fd=fd,
                         identity=(info.st_dev, info.st_ino),
                         requested=self.requested / name,
                         ancestors=self.ancestors + ((info.st_dev, info.st_ino),))

    @contextlib.contextmanager
    def file(self, name, create=False):
        component(name)
        if os.name == "nt":
            handle, _ = _win_open(self.path / name, create=create)
            try:
                fd = msvcrt.open_osfhandle(handle, os.O_BINARY |
                                          (os.O_WRONLY if create else os.O_RDONLY))
            except BaseException:
                _kernel.CloseHandle(handle)
                raise
        else:
            flags = os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
            flags |= os.O_WRONLY | os.O_CREAT | os.O_EXCL if create else os.O_RDONLY
            fd = os.open(name, flags, 0o600, dir_fd=self.fd)
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            os.close(fd)
            raise ValueError("expected a regular file")
        # Raw reads keep the physical read request within the caller's budget;
        # a buffered reader may otherwise prefetch beyond the one-byte probe.
        with os.fdopen(fd, "wb" if create else "rb", buffering=-1 if create else 0) as stream:
            yield stream

    def read(self, name, maximum, exact=None):
        with self.file(name) as stream:
            return read_bounded(stream, maximum, exact)

    def write(self, name, data):
        with self.file(name, create=True) as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())

    def publish(self, name, data):
        """Publish complete bytes without replacing any concurrent destination."""
        component(name)
        self.visible()
        self.require_absent(name)
        temporary = ".p6-report-" + uuid.uuid4().hex
        with self.child(temporary, create=True) as staged:
            staged.write("report.json", data)
            self.visible()
            if os.name == "nt":
                # os.rename refuses an existing destination on Windows.
                os.rename(staged.path / "report.json", self.path / name)
            else:
                os.link("report.json", name, src_dir_fd=staged.fd,
                        dst_dir_fd=self.fd, follow_symlinks=False)
        # The tiny private staging directory is intentionally retained, also
        # on failure. No pathname cleanup can remove a replacement entry in a
        # shared report parent. On POSIX its report is a second link to the
        # published immutable bytes; it is never reused by a later run.
