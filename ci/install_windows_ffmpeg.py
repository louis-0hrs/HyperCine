# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Install the Windows CI decoder tools without trusting Chocolatey's exit alone."""
import re
import shutil
import subprocess
import time


ATTEMPTS = 3
# Only recognized transport/feed failures justify another install attempt.
HTTP_STATUS = re.compile(
    r"(?<!\w)(?:HTTP(?:/\d(?:\.\d)?)?\s*[:=]?\s*|"
    r"status(?:\s+code)?\s*[:=]?\s*|"
    r"Response\s+status\s+code\s+does\s+not\s+indicate\s+success:\s*|"
    r"\()(429|5\d\d)\b", re.I)
TRANSPORT = re.compile(r"timed?\s*out|timeout|connection (?:reset|refused)|"
                       r"temporary failure in name resolution|remote name could not be resolved", re.I)


def probe_tools():
    """Check execution and version identity; an existing shim is insufficient."""
    ready = True
    for tool in ('ffmpeg', 'ffprobe'):
        executable = shutil.which(tool)
        works = False
        if executable:
            try:
                result = subprocess.run([executable, '-version'], capture_output=True,
                                        text=True, errors='replace', timeout=20)
                works = (result.returncode == 0 and
                         result.stdout.startswith(tool + ' version '))
            except (OSError, subprocess.TimeoutExpired):
                pass
        print(f'{tool}: {"functional" if works else "missing or unusable"}')
        ready = ready and works
    return ready


def install():
    for attempt in range(1, ATTEMPTS + 1):
        print(f'Installing package ffmpeg: attempt {attempt}/{ATTEMPTS}', flush=True)
        try:
            result = subprocess.run(['choco', 'install', 'ffmpeg', '--no-progress', '-y'],
                                    capture_output=True, text=True, errors='replace', timeout=600)
            output = result.stdout + result.stderr
            statuses = sorted(set(HTTP_STATUS.findall(output)))
            transient = bool(statuses or TRANSPORT.search(output))
            installer_ok = result.returncode == 0
            print(f'Chocolatey exit={result.returncode}; transient={transient}; '
                  f'HTTP={",".join(statuses) or "unreported"}')
        except subprocess.TimeoutExpired:
            transient, installer_ok = True, False
            print('Chocolatey: installation timed out')
        except OSError:
            # Never print raw exceptions/output: they can contain local paths.
            transient, installer_ok = False, False
            print('Chocolatey: cannot execute installer')

        tools_ok = probe_tools()
        if installer_ok and tools_ok:
            return 0
        if not transient or attempt == ATTEMPTS:
            print('FFmpeg installation gate failed')
            return 1
        print('Retrying transient FFmpeg installation failure in 5 seconds', flush=True)
        time.sleep(5)
    return 1


if __name__ == '__main__':
    raise SystemExit(install())
