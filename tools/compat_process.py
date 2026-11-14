# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Finite produced-output capture; Windows children enter a kill-on-close job.

Windows starts suspended so even a fast exiting parent cannot orphan a child
before job assignment. No breakaway flag is permitted. Assignment failure is
fail-closed, never a fallback to an uncontained subprocess.
"""
import ctypes
import os
import signal
import subprocess
import threading
import time


class ToolFailure(ValueError):
    pass


def _windows_job():
    from ctypes import wintypes as w
    class BASIC(ctypes.Structure):
        _fields_ = [('ProcessTime', ctypes.c_int64), ('JobTime', ctypes.c_int64),
                    ('Flags', w.DWORD), ('Min', ctypes.c_size_t), ('Max', ctypes.c_size_t),
                    ('Active', w.DWORD), ('Affinity', ctypes.c_size_t),
                    ('Priority', w.DWORD), ('Scheduling', w.DWORD)]
    class IO(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in
                    ('ReadOps', 'WriteOps', 'OtherOps', 'ReadBytes', 'WriteBytes', 'OtherBytes')]
    class EXTENDED(ctypes.Structure):
        _fields_ = [('Basic', BASIC), ('Io', IO), ('ProcessMemory', ctypes.c_size_t),
                    ('JobMemory', ctypes.c_size_t), ('PeakProcess', ctypes.c_size_t),
                    ('PeakJob', ctypes.c_size_t)]
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
    k.CreateJobObjectW.restype = w.HANDLE
    k.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
    k.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
    k.CloseHandle.argtypes = [w.HANDLE]
    job = k.CreateJobObjectW(None, None)
    if not job:
        raise ToolFailure('tool containment unavailable')
    limits = EXTENDED(); limits.Basic.Flags = 0x2000  # KILL_ON_JOB_CLOSE
    if not k.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
        k.CloseHandle(job)
        raise ToolFailure('tool containment unavailable')
    return k, job


def run_bounded(argv, timeout=15.0, stdout_limit=65536):
    if not (0 < timeout <= 120 and type(stdout_limit) is int and 0 < stdout_limit <= 134217728):
        raise ToolFailure('invalid tool budget')
    process = None; job = None; readers = []; output = bytearray()
    overflow = threading.Event(); failed = threading.Event()
    try:
        flags = {}
        if os.name == 'nt':
            kernel, job = _windows_job()
            flags['creationflags'] = 0x4  # CREATE_SUSPENDED
        else:
            flags['start_new_session'] = True
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, **flags)
        if os.name == 'nt':
            if not kernel.AssignProcessToJobObject(job, int(process._handle)):
                raise ToolFailure('tool containment unavailable')
            ntdll = ctypes.WinDLL('ntdll')
            ntdll.NtResumeProcess.argtypes = [ctypes.c_void_p]
            if ntdll.NtResumeProcess(int(process._handle)) != 0:
                raise ToolFailure('tool containment unavailable')
        def drain(stream, collect):
            count = 0
            try:
                while True:
                    chunk = stream.read(4096)
                    if not chunk:
                        break
                    count += len(chunk)
                    if count > stdout_limit:
                        overflow.set(); break
                    if collect:
                        output.extend(chunk)
            except (OSError, ValueError):
                failed.set()
        for stream, collect in ((process.stdout, True), (process.stderr, False)):
            thread = threading.Thread(target=drain, args=(stream, collect), daemon=True)
            readers.append(thread); thread.start()
        deadline = time.monotonic() + timeout
        while True:
            if overflow.is_set():
                raise ToolFailure('tool output limit exceeded')
            if failed.is_set():
                raise ToolFailure('tool capture failed')
            if process.poll() is not None and not any(t.is_alive() for t in readers):
                break
            if time.monotonic() >= deadline:
                raise ToolFailure('tool timeout')
            time.sleep(0.005)
        if process.returncode:
            raise ToolFailure('tool nonzero exit')
        return bytes(output)
    except (OSError, subprocess.SubprocessError):
        raise ToolFailure('tool launch failed') from None
    finally:
        if job is not None:
            kernel.CloseHandle(job)
        if process is not None:
            if os.name != 'nt':
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            elif process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            for thread in readers:
                thread.join(timeout=5)
            for stream in (process.stdout, process.stderr):
                stream.close()
            if any(t.is_alive() for t in readers):
                raise ToolFailure('tool cleanup failed')
