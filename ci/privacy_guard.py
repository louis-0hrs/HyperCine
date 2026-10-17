#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Fail-closed publication guard for added text in Git changes (stdlib-only).

Usage:
  python ci/privacy_guard.py --staged
  python ci/privacy_guard.py --commit <sha>
  python ci/privacy_guard.py --commits <base>..HEAD

Only reports filename, line and rule; never prints matching sensitive data.
It also scans commit messages and repository path names. Sensitive path names are
redacted in diagnostics. It does not validate binary blobs, issue text, GitHub
artifacts or old history.
"""
import argparse
import ipaddress
import re
import subprocess
import sys

IP_CANDIDATE = re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
# Match only RFC1918 address families without embedding real-looking hosts.
def is_private_network_address(address):
    a, b, _, _ = address.packed
    return a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168)
UNIX_HOME = re.compile(r"(?<![\w/])/(?:Users|home|root)/[^/\s\"'<>]+")
WINDOWS_HOME = re.compile(r"\b[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s\"'<>]+")
TILDE_HOME = re.compile(r"(?<![\w/])~/(?=[A-Za-z._])[^\s\"'<>]+")
WINDOWS_UNC = re.compile(r"(?<![A-Za-z0-9])\\{2,}[A-Za-z0-9._-]+\\+[A-Za-z0-9.$_-]+")
POSIX_SHARE = re.compile(r"(?<![A-Za-z0-9:/])//[A-Za-z0-9._-]+/[A-Za-z0-9.$_-]+")
NETWORK_URI = re.compile(r"\b(?:smb|cifs|nfs|sshfs)://", re.IGNORECASE)
PRIVATE_DNS = re.compile(r"\b[a-z0-9-]+(?:\.[a-z0-9-]+)*\.(?:local|lan|internal)\b",
                         re.IGNORECASE)
TOKEN = re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,}"
                   r"|AKIA[0-9A-Z]{16})\b")
CREDENTIAL = re.compile(
    r"\b(?:password|passwd|api[_-]?key|access[_-]?token|auth[_-]?token|"
    r"client[_-]?secret)\b\s*[:=]\s*['\"]?([^\s'\";,]+)", re.IGNORECASE)
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")
PLACEHOLDER = re.compile(
    r"^(?:<.*>|\$\{.*\}|\$[A-Z_][A-Z_0-9]*|[*xX]{3,}|"
    r"(?:example|placeholder|redacted|dummy|test|fake|change[_-]?me|none|null)"
    r"[\w.-]*)$", re.IGNORECASE)


def rules_for(line):
    """Return rule names without returning the private values."""
    result = set()
    for candidate in IP_CANDIDATE.findall(line):
        try:
            address = ipaddress.ip_address(candidate)
        except ValueError:
            continue
        if is_private_network_address(address):
            result.add("PRIVATE_IPV4")
    if UNIX_HOME.search(line) or WINDOWS_HOME.search(line) or TILDE_HOME.search(line):
        result.add("PERSONAL_ABSOLUTE_PATH")
    if WINDOWS_UNC.search(line) or POSIX_SHARE.search(line) or NETWORK_URI.search(line):
        result.add("NETWORK_SHARE")
    if PRIVATE_DNS.search(line):
        result.add("PRIVATE_HOSTNAME")
    if TOKEN.search(line):
        result.add("TOKEN_PATTERN")
    for match in CREDENTIAL.finditer(line):
        if not PLACEHOLDER.fullmatch(match.group(1)):
            result.add("POSSIBLE_CREDENTIAL")
    return sorted(result)


def scan_patch(patch):
    """Inspect added lines only; return (path, new_line, rule)."""
    findings = []
    path = None
    line_number = None
    for entry in patch.splitlines():
        if entry.startswith("+++ b/"):
            path = entry[6:]
            line_number = None
            for rule in rules_for(path):
                findings.append(("[SENSITIVE_PATH]", 0, "PATH_" + rule))
        elif entry.startswith("+++ /dev/null"):
            path = None
            line_number = None
        elif entry.startswith("@@ "):
            match = HUNK.match(entry)
            line_number = int(match.group(1)) if match else None
        elif entry.startswith("+") and not entry.startswith("+++"):
            if path is not None and line_number is not None:
                for rule in rules_for(entry[1:]):
                    findings.append(("[SENSITIVE_PATH]" if rules_for(path)
                                     else path, line_number, rule))
                line_number += 1
        elif entry.startswith(" ") and line_number is not None:
            line_number += 1
    return findings


def git(*args):
    proc = subprocess.run(["git", *args], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode:
        # Git stderr may itself contain the input ref or local paths. Redact it.
        raise RuntimeError("Git command failed; check repository and revision arguments")
    return proc.stdout


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--staged", action="store_true")
    group.add_argument("--commit", metavar="SHA")
    group.add_argument("--commits", metavar="BASE..HEAD")
    args = parser.parse_args(argv)
    # scan_patch requires b/ destination headers regardless of local Git settings.
    common = ("--no-ext-diff", "--no-color", "--src-prefix=a/", "--dst-prefix=b/",
              "--unified=0", "--diff-filter=ACMRT")
    patches = []
    commits = []
    if args.staged:
        patches.append(git("diff", "--cached", *common))
    elif args.commit:
        commits = [args.commit]
    else:
        commits = git("rev-list", "--reverse", "--topo-order", args.commits).splitlines()
        # Empty ranges are allowed (e.g. synchronized branches with no new commits).
    for sha in commits:
        patches.append(git("show", "--format=", "--first-parent", *common, sha))
    findings = [finding for patch in patches for finding in scan_patch(patch)]
    # A secret in a commit subject/body is public even when the patch is clean.
    for sha in commits:
        message = git("show", "-s", "--format=%B", sha)
        for rule in rules_for(message):
            findings.append(("[COMMIT_MESSAGE]", 0, rule))
    if findings:
        for path, line, rule in findings[:100]:
            print("{}:{}: {}".format(path, line, rule), file=sys.stderr)
        if len(findings) > 100:
            print("... additional violations omitted", file=sys.stderr)
        print("PRIVACY GUARD FAILED: remove or sanitize findings before publication.",
              file=sys.stderr)
        return 1
    print("Privacy guard passed (added text inspected; binary/old history not covered).")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, UnicodeError) as error:
        print("PRIVACY GUARD ERROR: unable to complete scan; publication blocked.",
              file=sys.stderr)
        sys.exit(2)
