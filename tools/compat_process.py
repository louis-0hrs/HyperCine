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
import selectors
import struct
import sys
import signal
import subprocess
import threading
import time
import uuid


class ToolFailure(ValueError):
    def __init__(self, message, cleanup_failed=False, containment_unconfirmed=False):
        super().__init__(message)
        self.primary_category = message
        self.cleanup_failed = cleanup_failed
        self.containment_unconfirmed = containment_unconfirmed


# Strong ownership is retained until a dedicated continuation reaps a child.
# No continuation ever signals a process group after the foreground wait.
_OWNED_REAP = set()
_OWNED_REAP_LOCK = threading.Lock()


def _retain_reap(process):
    with _OWNED_REAP_LOCK:
        _OWNED_REAP.add(process)
    def reap():
        try:
            process.wait()
        except BaseException:
            # Keep the object registered for explicit operator recovery.
            return
        with _OWNED_REAP_LOCK:
            _OWNED_REAP.discard(process)
    try:
        threading.Thread(target=reap, name='compat-owned-reap', daemon=True).start()
    except BaseException:
        # Strong ownership is still visible in the registry.
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
    k.CloseHandle.restype = w.BOOL
    k.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
    k.TerminateJobObject.restype = w.BOOL
    k.PeekNamedPipe.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                               ctypes.c_void_p, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
    k.PeekNamedPipe.restype = w.BOOL
    job = k.CreateJobObjectW(None, None)
    if not job:
        raise ToolFailure('tool containment unavailable')
    limits = EXTENDED(); limits.Basic.Flags = 0x2000  # KILL_ON_JOB_CLOSE
    setup_error = None
    try:
        configured = bool(k.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)))
    except BaseException as exc:
        configured = False
        setup_error = exc
    if not configured:
        close_error = None
        try:
            closed = bool(k.CloseHandle(job))
        except BaseException as exc:
            closed = False
            close_error = exc
        if not closed:
            _retain_job(k, job)
        for exc in (setup_error, close_error):
            if exc is not None and not isinstance(exc, Exception):
                raise exc
        raise ToolFailure('tool containment unavailable', not closed, not closed) from None
    return k, job


# Own a failed Job close until an explicit retry succeeds. The continuation
# never uses a process-group number; this handle is private to this process.
_OWNED_JOBS = set()
_OWNED_JOBS_LOCK = threading.Lock()


def _retain_job(kernel, job):
    owned = (kernel, job)
    with _OWNED_JOBS_LOCK:
        _OWNED_JOBS.add(owned)
    def dispose():
        try:
            kernel.TerminateJobObject(job, 1)
            if not kernel.CloseHandle(job):
                return
        except BaseException:
            return
        with _OWNED_JOBS_LOCK:
            _OWNED_JOBS.discard(owned)
    try:
        threading.Thread(target=dispose, name='compat-owned-job', daemon=True).start()
    except BaseException:
        pass


def _windows_resume(process):
    ntdll = ctypes.WinDLL('ntdll')
    ntdll.NtResumeProcess.argtypes = [ctypes.c_void_p]
    ntdll.NtResumeProcess.restype = ctypes.c_long
    return ntdll.NtResumeProcess(int(process._handle)) == 0


def _configure_windows_io(k):
    from ctypes import wintypes as w
    k.CreateNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.DWORD,
                                  w.DWORD, w.DWORD, w.DWORD, ctypes.c_void_p]
    k.CreateNamedPipeW.restype = w.HANDLE
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                              w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    k.CreateEventW.argtypes = [ctypes.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
    k.CreateEventW.restype = w.HANDLE
    k.ConnectNamedPipe.argtypes = [w.HANDLE, ctypes.c_void_p]
    k.ConnectNamedPipe.restype = w.BOOL
    k.ResetEvent.argtypes = [w.HANDLE]
    k.ReadFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                           ctypes.POINTER(w.DWORD), ctypes.c_void_p]
    k.ReadFile.restype = w.BOOL
    k.GetOverlappedResult.argtypes = [w.HANDLE, ctypes.c_void_p,
                                     ctypes.POINTER(w.DWORD), w.BOOL]
    k.GetOverlappedResult.restype = w.BOOL
    k.CancelIoEx.argtypes = [w.HANDLE, ctypes.c_void_p]
    k.CancelIoEx.restype = w.BOOL
    k.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    k.WaitForSingleObject.restype = w.DWORD
    k.GetCurrentProcess.argtypes = []
    k.GetCurrentProcess.restype = w.HANDLE
    k.DuplicateHandle.argtypes = [w.HANDLE, w.HANDLE, w.HANDLE,
                                  ctypes.POINTER(w.HANDLE), w.DWORD, w.BOOL, w.DWORD]
    k.DuplicateHandle.restype = w.BOOL


class _Overlapped(ctypes.Structure):
    _fields_ = [('Internal', ctypes.c_size_t), ('InternalHigh', ctypes.c_size_t),
                ('Offset', ctypes.c_uint32), ('OffsetHigh', ctypes.c_uint32),
                ('hEvent', ctypes.c_void_p)]


# A pending operation owns its buffer, OVERLAPPED, event and pipe until it
# completes, including after a failed cancellation or exhausted cleanup budget.
_OWNED_IO = set()
_OWNED_IO_LOCK = threading.Lock()


def _retain_io(pipe):
    with _OWNED_IO_LOCK:
        _OWNED_IO.add(pipe)
    def finish():
        try:
            if pipe.pending:
                pipe.kernel.WaitForSingleObject(pipe.event, 0xffffffff)
                pipe.finish_pending()
            pipe.close()
        except BaseException:
            return  # registry retains the handles and OVERLAPPED storage
        with _OWNED_IO_LOCK:
            _OWNED_IO.discard(pipe)
    try:
        threading.Thread(target=finish, name='compat-owned-io', daemon=True).start()
    except BaseException:
        pass


def _duplicate_writer(kernel, raw):
    from ctypes import wintypes as w
    process = kernel.GetCurrentProcess()
    duplicate = w.HANDLE()
    if not kernel.DuplicateHandle(process, raw, process, ctypes.byref(duplicate), 0, False, 2):
        raise OSError('writer witness unavailable')
    return duplicate.value


def _same_writer(kernel, raw, witness):
    # This API is unavailable on some older systems: retain rather than risk
    # closing a recycled handle when identity cannot be established.
    try:
        # CompareObjectHandles is exported by KernelBase, not necessarily
        # forwarded by Kernel32 on supported Windows installations.
        compare = ctypes.WinDLL('KernelBase', use_last_error=True).CompareObjectHandles
        from ctypes import wintypes as w
        compare.argtypes = [w.HANDLE, w.HANDLE]
        compare.restype = w.BOOL
        return bool(compare(raw, witness))
    except (AttributeError, OSError):
        return False


def _retry_owned_io(pipe):
    # Explicit safe continuation; close() checks writer identity before retry.
    pipe.close()
    with _OWNED_IO_LOCK:
        _OWNED_IO.discard(pipe)


class _WindowsPipe:
    def __init__(self, kernel):
        import msvcrt
        self.kernel = kernel
        self.close_lock = threading.RLock()
        self.handle = self.event = self.writer = self.writer_handle = None
        self.writer_borrowed = self.writer_witness = None
        self.writer_state = 'empty'
        self.writer_retry = False
        self.pending = False
        self.buffer = self.overlap = None
        _configure_windows_io(kernel)
        name = os.sep * 2 + '.' + os.sep + 'pipe' + os.sep + 'hypercine-' + uuid.uuid4().hex
        invalid = ctypes.c_void_p(-1).value
        try:
            # FILE_FLAG_OVERLAPPED + FILE_FLAG_FIRST_PIPE_INSTANCE, inbound.
            self.handle = kernel.CreateNamedPipeW(name, 0x40080001, 0x8, 1,
                                                   0, 65536, 0, None)
            if self.handle == invalid:
                self.handle = None
                raise OSError('pipe creation failed')
            self.writer_handle = kernel.CreateFileW(name, 0x40000000, 0, None, 3, 0, None)
            if self.writer_handle == invalid:
                self.writer_handle = None
                raise OSError('pipe connection failed')
            self.writer_state = 'raw'
            self.writer_witness = _duplicate_writer(kernel, self.writer_handle)
            self.writer_borrowed = self.writer_handle  # identity only; CRT owns it after adoption
            self.writer = msvcrt.open_osfhandle(self.writer_handle, os.O_WRONLY | os.O_BINARY)
            self.writer_state = 'crt'
            self.writer_handle = None  # never CloseHandle the CRT-owned handle
            self.event = kernel.CreateEventW(None, True, False, None)
            if not self.event:
                self.event = None  # NULL is not an owned handle.
                raise OSError('pipe event unavailable')
            connect = _Overlapped(); connect.hEvent = self.event
            # Connect may itself be interrupted after the kernel has accepted
            # the operation; pin its storage before entering the call.
            self.overlap = connect
            self.pending = True
            if not kernel.ConnectNamedPipe(self.handle, ctypes.byref(connect)):
                error = ctypes.get_last_error()
                if error != 535:  # ERROR_PIPE_CONNECTED: client already opened
                    raise OSError(error, 'pipe connection failed')
            self.pending = False
        except BaseException as original:
            cleanup_problem = False
            try:
                self.close_writer()
            except BaseException:
                cleanup_problem = True
            try:
                self.cancel()
            except BaseException:
                cleanup_problem = True
            if self.pending:
                cleanup_problem = True
                _retain_io(self)
            else:
                try:
                    self.close()
                except BaseException:
                    cleanup_problem = True
                    _retain_io(self)
            if cleanup_problem and isinstance(original, Exception):
                raise ToolFailure('tool capture failed', True, False) from None
            raise

    def close_writer(self):
        with self.close_lock:
            return self._close_writer_locked()

    def _close_writer_locked(self):
        import msvcrt
        k = self.kernel
        if self.writer_state == 'crt':
            fd = self.writer
            # If an earlier failure retained the fd, check it still points at
            # our anchored object; never blindly close a recycled CRT slot.
            if fd is None:
                raise ToolFailure('tool capture failed', True, False)
            if self.writer_retry and not _same_writer(k, msvcrt.get_osfhandle(fd), self.writer_witness):
                raise ToolFailure('tool capture failed', True, False)
            try:
                os.close(fd)
            except BaseException as exc:
                try:
                    current = msvcrt.get_osfhandle(fd)
                except OSError:
                    current = None
                if current is None or not _same_writer(k, current, self.writer_witness):
                    self.writer = None  # CRT slot retired/reused: never retry it
                    self.writer_handle = self.writer_borrowed
                    self.writer_borrowed = None
                    self.writer_state = 'retired'
                else:
                    self.writer_retry = True
                if not isinstance(exc, Exception):
                    raise
                raise ToolFailure('tool capture failed', True, False) from None
            self.writer = None
            self.writer_handle = None
            self.writer_borrowed = None
            self.writer_state = 'closed'
        elif self.writer_state == 'raw':
            if self.writer_handle is not None:
                if not k.CloseHandle(self.writer_handle):
                    raise ToolFailure('tool capture failed', True, False)
                self.writer_handle = None
            self.writer_state = 'closed'
        elif self.writer_state == 'retired':
            # The original fd is gone. Its former raw handle is recoverable
            # only when our independently owned witness proves object identity.
            if not _same_writer(k, self.writer_handle, self.writer_witness):
                raise ToolFailure('tool capture failed', True, False)
            if not k.CloseHandle(self.writer_handle):
                raise ToolFailure('tool capture failed', True, False)
            self.writer_handle = None
            self.writer_state = 'closed'
        if self.writer_witness is not None and self.writer_state == 'closed':
            if not k.CloseHandle(self.writer_witness):
                raise ToolFailure('tool capture failed', True, False)
            self.writer_witness = None

    def read_ready(self):
        from ctypes import wintypes as w
        k = self.kernel
        if not self.pending:
            if not k.ResetEvent(self.event):
                raise OSError('pipe event reset failed')
            self.overlap = _Overlapped(); self.overlap.hEvent = self.event
            self.buffer = ctypes.create_string_buffer(4096)
            # Pin before submission: interruption at the call boundary cannot
            # release storage for an operation which may have been issued.
            self.pending = True
            if not k.ReadFile(self.handle, self.buffer, 4096, None,
                              ctypes.byref(self.overlap)):
                error = ctypes.get_last_error()
                if error in (109, 232):
                    self.pending = False
                    return None
                if error != 997:  # ERROR_IO_PENDING
                    raise OSError(error, 'pipe read failed')
        n = w.DWORD()
        if not k.GetOverlappedResult(self.handle, ctypes.byref(self.overlap), ctypes.byref(n), False):
            error = ctypes.get_last_error()
            if error == 996:  # ERROR_IO_INCOMPLETE
                return b''
            if error in (109, 232, 995):
                self.pending = False
                return None
            # Unknown completion state: keep OVERLAPPED and buffer pinned.
            raise OSError(error, 'pipe completion failed')
        self.pending = False
        return self.buffer.raw[:n.value] if n.value else None

    def cancel(self):
        if self.pending and not self.kernel.CancelIoEx(self.handle, ctypes.byref(self.overlap)):
            if ctypes.get_last_error() != 1168:  # already completed
                raise OSError('pipe cancellation failed')

    def finish_pending(self):
        if self.pending:
            self.read_ready()
        return not self.pending

    def close(self):
        with self.close_lock:
            return self._close_locked()

    def _close_locked(self):
        failed = False
        try:
            self.close_writer()
        except BaseException:
            failed = True
        if self.pending:
            raise OSError('pipe operation still pending')
        for attr in ('handle', 'event'):
            handle = getattr(self, attr)
            if handle is not None:
                try:
                    closed = self.kernel.CloseHandle(handle)
                except BaseException:
                    failed = True
                    continue
                if closed:
                    setattr(self, attr, None)
                else:
                    failed = True
        if failed:
            raise OSError('pipe cleanup failed')


def _windows_open_pipe(kernel):
    return _WindowsPipe(kernel)


def _run_windows(argv, timeout, stdout_limit):
    process = kernel = job = None
    streams = []
    output = bytearray()
    primary = pending_interrupt = None
    cleanup_failed = containment_unconfirmed = False
    assigned = False
    counts = [0, 0]

    def cleanup_error(exc, containment=False):
        nonlocal cleanup_failed, containment_unconfirmed, pending_interrupt
        cleanup_failed = True
        containment_unconfirmed = containment_unconfirmed or containment
        if not isinstance(exc, Exception) and pending_interrupt is None:
            pending_interrupt = exc

    try:
        kernel, job = _windows_job()
        streams.append(_windows_open_pipe(kernel))
        streams.append(_windows_open_pipe(kernel))
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=streams[0].writer,
                                   stderr=streams[1].writer, creationflags=0x4, bufsize=0)
        for stream in streams:
            stream.close_writer()
        if not kernel.AssignProcessToJobObject(job, int(process._handle)):
            raise ToolFailure('tool containment unavailable')
        assigned = True
        if not _windows_resume(process):
            raise ToolFailure('tool containment unavailable')
        deadline = time.monotonic() + timeout
        active = [True, True]
        while True:
            progress = False
            for index, stream in enumerate(streams):
                if not active[index]:
                    continue
                chunk = stream.read_ready()
                if chunk is None:
                    active[index] = False
                elif chunk:
                    progress = True
                    counts[index] += len(chunk)
                    if counts[index] > stdout_limit:
                        raise ToolFailure('tool output limit exceeded')
                    if index == 0:
                        output.extend(chunk)
            if process.poll() is not None and not any(active):
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ToolFailure('tool timeout')
            if not progress:
                time.sleep(min(.005, remaining))
        if process.returncode:
            raise ToolFailure('tool nonzero exit')
    except ToolFailure as exc:
        primary = str(exc)
        cleanup_failed = exc.cleanup_failed
        containment_unconfirmed = exc.containment_unconfirmed
    except Exception:
        primary = 'tool launch failed' if process is None else 'tool capture failed'
    except BaseException as exc:
        pending_interrupt = exc
    finally:
        end = time.monotonic() + 2.0
        if job is not None:
            try:
                if not kernel.CloseHandle(job):
                    raise OSError('job close failed')
                job = None
            except BaseException as exc:
                cleanup_error(exc, containment=True)
                # If close failed the original job remains owned; request
                # termination independently, then attempt one bounded close.
                try:
                    if not kernel.TerminateJobObject(job, 1):
                        raise OSError('job termination failed')
                except BaseException as fallback_error:
                    cleanup_error(fallback_error, containment=True)
                try:
                    if kernel.CloseHandle(job):
                        job = None
                except BaseException as close_error:
                    cleanup_error(close_error, containment=True)
                if job is not None:
                    _retain_job(kernel, job)
        if process is not None:
            try:
                running = process.poll() is None
            except BaseException as exc:
                cleanup_error(exc, containment=assigned)
                running = True  # Preserve the safe owned-handle fallback.
            if running:
                try:
                    process.kill()
                except BaseException as exc:
                    cleanup_error(exc, containment=assigned)
            reaped = False
            while True:
                try:
                    process.wait(timeout=max(0.0, end - time.monotonic()))
                    reaped = True
                    break
                except BaseException as exc:
                    cleanup_error(exc, containment=assigned)
                    if process.returncode is not None:
                        reaped = True
                        break
                    if isinstance(exc, Exception) or time.monotonic() >= end:
                        break
            if not reaped:
                _retain_reap(process)
        for stream in streams:
            try:
                stream.close_writer()
            except BaseException as exc:
                cleanup_error(exc)
        for stream in streams:
            try:
                stream.cancel()
            except BaseException as exc:
                cleanup_error(exc)
        for stream in streams:
            try:
                while stream.pending and time.monotonic() < end:
                    stream.finish_pending()
                    if stream.pending:
                        time.sleep(min(.005, max(0.0, end - time.monotonic())))
                if stream.pending:
                    cleanup_failed = True
                    _retain_io(stream)
                else:
                    stream.close()
            except BaseException as exc:
                cleanup_error(exc)
                _retain_io(stream)
    if pending_interrupt is not None:
        raise pending_interrupt
    if primary is not None or cleanup_failed:
        message = primary or 'tool cleanup failed'
        if primary is not None and cleanup_failed:
            message += '; tool cleanup failed'
        failure = ToolFailure(message, cleanup_failed, containment_unconfirmed)
        failure.primary_category = primary
        raise failure from None
    return bytes(output)


def _supervise(control_fd, argv):
    """Session leader remains alive until the caller kills its anchored group."""
    os.set_inheritable(control_fd, False)
    try:
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, close_fds=True)
    except (OSError, ValueError, subprocess.SubprocessError):
        os.close(1)
        os.close(2)
        os.write(control_fd, b'E')
    else:
        # Only the actual tool and its descendants should hold capture pipes.
        os.close(1)
        os.close(2)
        code = child.wait()
        os.write(control_fd, b'R' + struct.pack('!i', code))
    # Keep the private channel open as a liveness witness until group cleanup.
    while True:
        signal.pause()


def _run_posix(argv, timeout, stdout_limit):
    anchor = None
    control_r = control_w = None
    streams = []
    selector = None
    output = bytearray()
    primary = None
    pending_interrupt = None
    cleanup_failed = False
    containment_unconfirmed = False
    counts = {'out': 0, 'err': 0}
    status = bytearray()

    def cleanup_error(exc, containment=False):
        nonlocal cleanup_failed, containment_unconfirmed, pending_interrupt
        cleanup_failed = True
        containment_unconfirmed = containment_unconfirmed or containment
        if not isinstance(exc, Exception) and pending_interrupt is None:
            pending_interrupt = exc

    try:
        control_r, control_w = os.pipe()
        command = [argv] if isinstance(argv, (str, bytes, os.PathLike)) else list(argv)
        anchor = subprocess.Popen(
            [sys.executable, '-B', os.path.abspath(__file__), '--supervise',
             str(control_w)] + command, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
            close_fds=True, pass_fds=(control_w,), start_new_session=True)
        streams = [anchor.stdout, anchor.stderr]
        os.close(control_w)
        control_w = None
        selector = selectors.DefaultSelector()
        for stream, name in ((anchor.stdout, 'out'), (anchor.stderr, 'err')):
            os.set_blocking(stream.fileno(), False)
            selector.register(stream, selectors.EVENT_READ, name)
        os.set_blocking(control_r, False)
        selector.register(control_r, selectors.EVENT_READ, 'status')
        deadline = time.monotonic() + timeout
        while True:
            captures_done = not any(key.data != 'status' for key in selector.get_map().values())
            if len(status) == 5 and captures_done:
                # A still-open status channel witnesses the trusted leader.
                # Check queued EOF or unexpected extra bytes before signaling.
                events = selector.select(0)
                if not events:
                    break
            else:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise ToolFailure('tool timeout')
                events = selector.select(remaining)
            for key, _ in events:
                try:
                    chunk = os.read(key.fd, 4096)
                except BlockingIOError:
                    continue
                if not chunk:
                    selector.unregister(key.fileobj)
                    if key.data == 'status':
                        raise ToolFailure('tool containment unavailable')
                    continue
                if key.data == 'status':
                    status.extend(chunk)
                    if status == b'E':
                        raise ToolFailure('tool launch failed')
                    if len(status) > 5 or status[0:1] != b'R':
                        raise ToolFailure('tool containment unavailable')
                else:
                    counts[key.data] += len(chunk)
                    if counts[key.data] > stdout_limit:
                        raise ToolFailure('tool output limit exceeded')
                    if key.data == 'out':
                        output.extend(chunk)
        if status == b'E':
            raise ToolFailure('tool launch failed')
        if len(status) != 5 or status[0:1] != b'R':
            raise ToolFailure('tool containment unavailable')
        if struct.unpack('!i', status[1:])[0] != 0:
            raise ToolFailure('tool nonzero exit')
    except ToolFailure as exc:
        primary = str(exc)
    except Exception:
        primary = 'tool launch failed' if anchor is None else 'tool capture failed'
    except BaseException as exc:
        pending_interrupt = exc
    finally:
        # Never inspect/reap the anchor until AFTER the one group signal. Its
        # unreaped PID pins group identity even if it unexpectedly died.
        end = time.monotonic() + 2.0
        if anchor is not None:
            try:
                os.killpg(anchor.pid, signal.SIGKILL)
            except BaseException as exc:
                cleanup_error(exc, containment=True)
                # This is still our unreaped child, not a recycled group ID.
                try:
                    anchor.kill()
                except BaseException as fallback_error:
                    cleanup_error(fallback_error, containment=True)
            reaped = False
            while True:
                try:
                    anchor_code = anchor.wait(timeout=max(0.0, end - time.monotonic()))
                    reaped = True
                    if anchor_code != -signal.SIGKILL:
                        # Status does not prove the anchor survived to signal.
                        cleanup_failed = containment_unconfirmed = True
                    break
                except BaseException as exc:
                    cleanup_error(exc, containment=True)
                    # Preserve first interrupt, retry only this owned child,
                    # and never repeat a group signal after any wait.
                    if anchor.returncode is not None:
                        reaped = True
                        if anchor.returncode != -signal.SIGKILL:
                            cleanup_failed = containment_unconfirmed = True
                        break
                    if isinstance(exc, Exception) or time.monotonic() >= end:
                        break
            if not reaped:
                _retain_reap(anchor)
        if selector is not None:
            try:
                selector.close()
            except BaseException as exc:
                cleanup_error(exc)
        for stream in streams:
            try:
                stream.close()
            except BaseException as exc:
                cleanup_error(exc)
        for fd in (control_r, control_w):
            if fd is not None:
                try:
                    os.close(fd)
                except BaseException as exc:
                    cleanup_error(exc)
    if pending_interrupt is not None:
        raise pending_interrupt
    if primary is not None or cleanup_failed:
        message = primary or 'tool cleanup failed'
        if cleanup_failed and primary is not None:
            message += '; tool cleanup failed'
        failure = ToolFailure(message, cleanup_failed, containment_unconfirmed)
        failure.primary_category = primary
        raise failure from None
    return bytes(output)


def run_bounded(argv, timeout=15.0, stdout_limit=65536):
    if not (0 < timeout <= 120 and type(stdout_limit) is int and 0 < stdout_limit <= 134217728):
        raise ToolFailure('invalid tool budget')
    if os.name == 'nt':
        return _run_windows(argv, timeout, stdout_limit)
    return _run_posix(argv, timeout, stdout_limit)


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == '--supervise':
        _supervise(int(sys.argv[2]), sys.argv[3:])
