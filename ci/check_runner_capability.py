#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Assert that the host actually matches the capability label it was routed by.

A self-hosted runner is selected by labels, not by its real hardware. If a host
is mislabelled, an x86 host can green an `hc-macos-arm64` lane and nothing in the
build output would say so. This check runs first on every self-hosted lane and
fails closed on a mismatch.

Deliberately dependency-free (stdlib only) and identical on Linux, macOS and
Windows, because it runs before the build tooling is trusted.

    python3 ci/check_runner_capability.py --capability hc-linux-x64-cpu
"""

from __future__ import annotations

import argparse
import platform
import shutil
import sys

# capability label -> (accepted sys.platform prefixes, accepted machine names, required tools)
CAPABILITIES: dict[str, tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]] = {
    "hc-linux-x64-cpu": (("linux",), ("x86_64", "amd64"), ("cmake", "ninja", "ffmpeg", "git")),
    "hc-linux-x64-cuda12.8": (
        ("linux",),
        ("x86_64", "amd64"),
        ("cmake", "ninja", "git", "gcc", "g++"),
    ),
    "hc-windows-x64-msvc": (
        # platform.system() is "Windows"; the others cover MSYS/Cygwin shells.
        ("windows", "win32", "cygwin", "msys"),
        ("amd64", "x86_64"),
        ("cmake", "ninja", "ffmpeg", "git"),
    ),
    "hc-macos-arm64": (("darwin",), ("arm64", "aarch64"), ("cmake", "ninja", "ffmpeg", "git")),
}


def check(capability: str, system: str, machine: str, which=shutil.which) -> list[str]:
    """Return a list of human-readable problems; empty means the host matches."""
    if capability not in CAPABILITIES:
        return [f"unknown capability label {capability!r}"]
    systems, machines, tools = CAPABILITIES[capability]
    problems: list[str] = []
    system_l = system.lower()
    if not any(system_l.startswith(prefix) for prefix in systems):
        problems.append(
            f"system mismatch: {system!r} does not match {capability} ({'/'.join(systems)})"
        )
    machine_l = machine.lower()
    if machine_l not in machines:
        problems.append(
            f"machine mismatch: {machine!r} does not match {capability} ({'/'.join(machines)})"
        )
    for tool in tools:
        if which(tool) is None:
            problems.append(f"missing required tool: {tool}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capability", required=True)
    parser.add_argument("--allow-missing", default="", help="comma-separated tools to ignore")
    args = parser.parse_args(argv)

    system = platform.system()
    machine = platform.machine()
    print(f"capability={args.capability} system={system} machine={machine}")

    # `--allow-missing` downgrades a missing tool to informational instead of
    # turning it into a false mismatch; the check must not report a tool as
    # missing when the operator explicitly waived it.
    ignored = {item.strip() for item in args.allow_missing.split(",") if item.strip()}

    problems = check(args.capability, system, machine, shutil.which)
    problems = [p for p in problems if not any(f": {tool}" in p for tool in ignored)]
    for problem in problems:
        print(f"MISMATCH {problem}", file=sys.stderr)
    if problems:
        print("runner capability check FAILED", file=sys.stderr)
        return 2
    print("runner capability check OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
