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
It also scans commit messages, author/committer identities and repository paths.
Staged scans check effective Git identities, including environment overrides.
Sensitive path names are redacted in diagnostics. It does not validate binary
blobs, issue text, GitHub artifacts or old history.
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
# Git Bash/MSYS mounts Windows drives with POSIX-like home paths.
MSYS_HOME = re.compile(r"(?<![\w/])/[a-z]/" r"Users/[^/\s\"'<>]+", re.IGNORECASE)
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
    if (UNIX_HOME.search(line) or WINDOWS_HOME.search(line) or
            MSYS_HOME.search(line) or TILDE_HOME.search(line)):
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


# Public dynamic/reverse-DNS patterns are heuristics only: the original
# multi-label public hostname disclosure did NOT end in .local/.internal.
HOST_DERIVED_DNS = re.compile(
    r"(?:^|[.-])(?:dip[0-9]+|dyn(?:dns|host|amic)?[0-9]*|dhcp[0-9]*|"
    r"pppoe[0-9]*|cpe[0-9]*|rev(?:erse)?dns|ip-[0-9]+)(?:$|[.-])",
    re.IGNORECASE)
AGENT_NAME = re.compile(r"\b(?:agent|openclaw|codex|automation)\b", re.IGNORECASE)
APPROVED_AGENT_DOMAINS = frozenset(("example.invalid", "users.noreply.github.com"))


def identity_findings(role, name, email):
    """Inspect real Git envelope identities without exposing their values."""
    rules = set(rules_for(name)) | set(rules_for(email))
    domain = email.rpartition("@")[2].lower()
    if not domain or "." not in domain or domain.endswith(".(none)"):
        rules.add("HOST_DERIVED_IDENTITY")
    if HOST_DERIVED_DNS.search(domain):
        rules.add("HOST_DERIVED_IDENTITY")
    if AGENT_NAME.search(name) and domain not in APPROVED_AGENT_DOMAINS:
        rules.add("UNAPPROVED_AGENT_IDENTITY")
    return [("[" + role + "]", 0, rule) for rule in sorted(rules)]


def configured_identity_field(field):
    """Read explicit Git configuration rather than git-var host inference."""
    proc = subprocess.run(["git", "config", "--get", field],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding="utf-8", errors="replace")
    if proc.returncode not in (0, 1):
        raise RuntimeError("Unable to read explicit Git identity configuration")
    return proc.stdout.strip() if proc.returncode == 0 else ""


def staged_identity_findings():
    # Missing explicit identity lets Git infer name/email from host and DNS.
    name = configured_identity_field("user.name")
    email = configured_identity_field("user.email")
    if not name or not email:
        return [("[AUTHOR]", 0, "UNCONFIGURED_IDENTITY"),
                ("[COMMITTER]", 0, "UNCONFIGURED_IDENTITY")]
    findings = []
    if AGENT_NAME.search(name) and configured_identity_field(
            "user.useConfigOnly").lower() not in ("true", "yes", "on", "1"):
        findings.append(("[AUTHOR]", 0, "AGENT_USE_CONFIG_ONLY_REQUIRED"))
    for role in ("AUTHOR", "COMMITTER"):
        identity = git("var", "GIT_" + role + "_IDENT").rstrip("\n")
        match = re.fullmatch(r"(.*) <([^<>]*)> \d+ [+-]\d{4}", identity)
        if not match:
            raise RuntimeError("Unable to parse effective Git identity")
        findings.extend(identity_findings(role, *match.groups()))
    return findings


def commit_identity_findings(sha):
    # Lowercase format codes return actual object fields, without mailmap.
    fields = git("show", "-s", "--format=%an%x00%ae%x00%cn%x00%ce",
                 sha).rstrip("\n").split("\0")
    if len(fields) != 4:
        raise RuntimeError("Unable to parse committed Git identity")
    return (identity_findings("AUTHOR", *fields[:2]) +
            identity_findings("COMMITTER", *fields[2:]))


def decode_patch_path(value):
    """Decode Git's C-quoted byte paths without evaluating arbitrary text."""
    if not value.startswith('"'):
        return value.split("\t", 1)[0]
    escaped = {"a": 7, "b": 8, "t": 9, "n": 10, "v": 11,
               "f": 12, "r": 13, "\\": 92, '"': 34}
    data = bytearray()
    index = 1
    while index < len(value):
        char = value[index]
        index += 1
        if char == '"':
            if value[index:] not in ("", "\t"):
                raise RuntimeError("Invalid Git patch path")
            return data.decode("utf-8", errors="replace")
        if char != "\\":
            data.extend(char.encode("utf-8"))
            continue
        if index >= len(value):
            break
        char = value[index]
        index += 1
        if char in escaped:
            data.append(escaped[char])
        elif char in "0123" and re.fullmatch(r"[0-7]{2}", value[index:index + 2]):
            data.append(int(char + value[index:index + 2], 8))
            index += 2
        else:
            raise RuntimeError("Invalid Git patch path escape")
    raise RuntimeError("Unterminated Git patch path")


def display_path(path):
    # Control characters in valid filenames must not forge diagnostic lines.
    return path.encode("unicode_escape").decode("ascii")


def scan_patch(patch):
    """Scan added lines, including +++-prefixed content inside a hunk."""
    findings = []
    path = None
    line_number = None
    for entry in patch.splitlines():
        if entry.startswith("diff --git "):
            path = None
            line_number = None
        elif line_number is None and (
                entry.startswith("+++ b/") or entry.startswith('+++ "b/')):
            decoded = decode_patch_path(entry[4:])
            if not decoded.startswith("b/"):
                raise RuntimeError("Invalid patch destination header")
            path = decoded[2:]
            for rule in rules_for(path):
                findings.append(("[SENSITIVE_PATH]", 0, "PATH_" + rule))
        elif line_number is None and entry.startswith("+++ /dev/null"):
            path = None
        elif entry.startswith("@@ "):
            match = HUNK.match(entry)
            if not match or path is None:
                raise RuntimeError("Unrecognized patch hunk")
            line_number = int(match.group(1))
        elif line_number is not None and entry.startswith("+"):
            # In a hunk, +++ b/whatever is source content, never a header.
            for rule in rules_for(entry[1:]):
                findings.append(("[SENSITIVE_PATH]" if rules_for(path)
                                 else display_path(path), line_number, rule))
            line_number += 1
        elif line_number is not None and entry.startswith(" "):
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
    # Enforce stable diff prefixes and bypass any textconv redaction.
    common = ("--no-ext-diff", "--no-textconv", "--no-color",
              "--src-prefix=a/", "--dst-prefix=b/", "--unified=0",
              "--diff-filter=ACMRT")
    patches = []
    paths = []
    commits = []
    findings = []
    if args.staged:
        findings.extend(staged_identity_findings())
        patches.append(git("diff", "--cached", *common))
        paths.extend(git("diff", "--cached", "--name-only", "-z",
                         "--no-renames", "--diff-filter=ACMRT").split("\0"))
    elif args.commit:
        commits = [args.commit]
    else:
        commits = git("rev-list", "--reverse", "--topo-order", args.commits).splitlines()
        # Empty ranges are allowed (e.g. synchronized branches with no new commits).
    for sha in commits:
        patches.append(git("show", "--format=", "--first-parent", *common, sha))
        paths.extend(git("show", "--format=", "--first-parent", "--name-only",
                         "-z", "--no-renames", "--diff-filter=ACMRT", sha).split("\0"))
    findings.extend(finding for patch in patches for finding in scan_patch(patch))
    # Binary, empty or rename-only changes have no textual +++ destination.
    for path in sorted(set(paths) - {""}):
        for rule in rules_for(path):
            item = ("[SENSITIVE_PATH]", 0, "PATH_" + rule)
            if item not in findings:
                findings.append(item)
    # A secret in a commit subject/body is public even when the patch is clean.
    for sha in commits:
        findings.extend(commit_identity_findings(sha))
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
