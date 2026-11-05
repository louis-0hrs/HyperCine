#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Static event-matrix verifier for the GitHub Actions routing in .github/workflows.

Why this exists
---------------
Moving Mac lanes through a private relay is a *routing* change: it decides which host
executes which required check. Nothing about a YAML edit proves that

  * the mandatory publication-privacy check still runs on every push and PR,
  * no public workflow can request a self-hosted runner,
  * a lane did not silently acquire the wrong platform capability label,
  * a paths filter still suppresses the expensive matrix for a docs-only PR.

This script answers those questions locally, without dispatching a single
hosted run. It parses the real workflow files, applies synthetic events, and
resolves `if:` and `runs-on:` expressions the way Actions documents them.

It is deliberately *not* a general Actions engine. Unsupported expression
syntax raises `UnsupportedExpression` and fails the run closed instead of
guessing.

Usage
-----
    python3 ci/workflow_event_matrix.py                  # verify the contract
    python3 ci/workflow_event_matrix.py --self-test      # mutation negative controls
    python3 ci/workflow_event_matrix.py --list           # resolved routing table

Exit status 0 means every rule held. Non-zero means at least one rule broke;
each violation is printed on its own line prefixed with `VIOLATION`.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import sys
from dataclasses import dataclass, field, replace
from typing import Any, Iterable

try:
    import yaml
except ImportError:  # pragma: no cover - the hosted jobs install PyYAML
    yaml = None

WORKFLOW_DIR = os.path.join(".github", "workflows")

# Capability labels this repository is allowed to request. A runner may carry
# several labels; these are the ones that pin an OS/arch/toolkit contract.
CAPABILITY_LABELS = {
    "hc-linux-x64-cpu": "Linux x86_64, no GPU, FFmpeg + Ninja + CMake available",
    "hc-linux-x64-cuda12.8": "Linux x86_64 with the pinned CUDA 12.8 toolkit",
    "hc-windows-x64-msvc": "Windows x86_64 with MSVC 19.x and Git for Windows",
    "hc-macos-arm64": "macOS arm64 with Xcode command line tools",
}
PLATFORM_PREFIX = "hc-"

# The required status check whose scope must never narrow.
PRIVACY_JOB = "publication-privacy"
PRIVACY_WORKFLOW = "privacy-guard.yml"
HOSTED_OS_LABELS = {
    "ubuntu-24.04",
    "ubuntu-latest",
    "ubuntu-22.04",
    "windows-2022",
    "windows-latest",
    "macos-15",
    "macos-14",
    "macos-latest",
}


class UnsupportedExpression(Exception):
    """Raised instead of guessing at syntax this verifier does not model."""


# --------------------------------------------------------------------------
# Expression subset
# --------------------------------------------------------------------------
# Supported: string/number/bool literals, context paths (github.x,
# matrix.name, inputs.y), property access, `!`, `&&`, `||`, `==`, `!=`,
# parentheses. `&&`/`||` return operands (JavaScript semantics), which is what
# makes the `cond && a || b` fallback idiom work.


@dataclass
class Tok:
    kind: str
    text: str


def _tokenize(src: str) -> list[Tok]:
    toks: list[Tok] = []
    i = 0
    while i < len(src):
        c = src[i]
        if c.isspace():
            i += 1
            continue
        if c in "'\"":
            quote = c
            j = i + 1
            buf = []
            while j < len(src):
                if src[j] == "\\" and j + 1 < len(src):
                    buf.append(src[j + 1])
                    j += 2
                    continue
                if src[j] == quote:
                    break
                buf.append(src[j])
                j += 1
            if j >= len(src):
                raise UnsupportedExpression(f"unterminated string in {src!r}")
            toks.append(Tok("str", "".join(buf)))
            i = j + 1
            continue
        if src.startswith("&&", i) or src.startswith("||", i) or src.startswith("==", i) or src.startswith("!=", i):
            toks.append(Tok("op", src[i : i + 2]))
            i += 2
            continue
        if c in "()!.,[]":
            toks.append(Tok("op", c))
            i += 1
            continue
        if c.isdigit():
            j = i
            while j < len(src) and (src[j].isdigit() or src[j] == "."):
                j += 1
            toks.append(Tok("num", src[i:j]))
            i = j
            continue
        if c.isalpha() or c in "_$":
            j = i
            while j < len(src) and (src[j].isalnum() or src[j] in "_-"):
                j += 1
            toks.append(Tok("ident", src[i:j]))
            i = j
            continue
        raise UnsupportedExpression(f"unexpected character {c!r} in {src!r}")
    return toks


def _fn_contains(args: list[Any], src: str) -> bool:
    if len(args) != 2:
        raise UnsupportedExpression(f"contains() takes 2 arguments in {src!r}")
    haystack, needle = args
    if isinstance(haystack, list):
        return any(str(item) == str(needle) for item in haystack)
    return str(needle) in str(haystack or "")


def _fn_from_json(args: list[Any], src: str) -> Any:
    if len(args) != 1:
        raise UnsupportedExpression(f"fromJSON() takes 1 argument in {src!r}")
    import json as _json

    try:
        return _json.loads(str(args[0]))
    except ValueError as exc:
        raise UnsupportedExpression(f"fromJSON() got invalid JSON in {src!r}: {exc}") from exc


def _fn_format(args: list[Any], src: str) -> str:
    if not args:
        raise UnsupportedExpression(f"format() needs a template in {src!r}")
    template = str(args[0])
    try:
        return template.format(*[("" if a is None else a) for a in args[1:]])
    except (IndexError, KeyError) as exc:
        raise UnsupportedExpression(f"format() placeholder mismatch in {src!r}: {exc}") from exc


_FUNCTIONS = {
    "contains": _fn_contains,
    "fromJSON": _fn_from_json,
    "format": _fn_format,
}


class _Parser:
    def __init__(self, toks: list[Tok], ctx: dict[str, Any], src: str):
        self.toks = toks
        self.i = 0
        self.ctx = ctx
        self.src = src

    def peek(self) -> Tok | None:
        return self.toks[self.i] if self.i < len(self.toks) else None

    def take(self) -> Tok:
        tok = self.peek()
        if tok is None:
            raise UnsupportedExpression(f"unexpected end of {self.src!r}")
        self.i += 1
        return tok

    def parse(self) -> Any:
        value = self.parse_or()
        if self.peek() is not None:
            raise UnsupportedExpression(f"trailing tokens in {self.src!r}")
        return value

    def parse_or(self) -> Any:
        left = self.parse_and()
        while self.peek() and self.peek().text == "||":
            self.take()
            right = self.parse_and()
            left = left if _truthy(left) else right
        return left

    def parse_and(self) -> Any:
        left = self.parse_cmp()
        while self.peek() and self.peek().text == "&&":
            self.take()
            right = self.parse_cmp()
            left = right if _truthy(left) else left
        return left

    def parse_cmp(self) -> Any:
        left = self.parse_unary()
        while self.peek() and self.peek().text in ("==", "!="):
            op = self.take().text
            right = self.parse_unary()
            equal = _loose_equal(left, right)
            left = equal if op == "==" else not equal
        return left

    def parse_unary(self) -> Any:
        tok = self.peek()
        if tok and tok.text == "!":
            self.take()
            return not _truthy(self.parse_unary())
        return self.parse_atom()

    def parse_atom(self) -> Any:
        tok = self.take()
        if tok.text == "(":
            value = self.parse_or()
            closing = self.take()
            if closing.text != ")":
                raise UnsupportedExpression(f"expected ')' in {self.src!r}")
            return value
        if tok.kind == "str":
            return tok.text
        if tok.kind == "num":
            return int(tok.text) if tok.text.isdigit() else float(tok.text)
        if tok.kind == "ident":
            if tok.text in _FUNCTIONS and self.peek() and self.peek().text == "(":
                self.take()
                args: list[Any] = []
                if not (self.peek() and self.peek().text == ")"):
                    while True:
                        args.append(self.parse_or())
                        sep = self.take()
                        if sep.text == ")":
                            break
                        if sep.text != ",":
                            raise UnsupportedExpression(
                                f"expected ',' or ')' in {self.src!r}"
                            )
                return _FUNCTIONS[tok.text](args, self.src)
            if tok.text in ("true", "True"):
                return True
            if tok.text in ("false", "False"):
                return False
            if tok.text in ("null", "None"):
                return None
            value = _lookup(self.ctx, tok.text)
            # property / index access chains
            while True:
                nxt = self.peek()
                if nxt is None:
                    break
                if nxt.text == ".":
                    self.take()
                    prop = self.take()
                    if prop.kind != "ident":
                        raise UnsupportedExpression(f"bad property in {self.src!r}")
                    if isinstance(value, dict):
                        value = value.get(prop.text)
                    else:
                        value = None
                    continue
                break
            return value
        raise UnsupportedExpression(f"unexpected token {tok.text!r} in {self.src!r}")


def _lookup(ctx: dict[str, Any], path: str) -> Any:
    value: Any = ctx
    for part in path.split("."):
        if isinstance(value, dict):
            value = value.get(part)
        else:
            return None
    return value


def _truthy(value: Any) -> bool:
    if value is None or value is False:
        return False
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return value != 0
    if isinstance(value, str):
        return value != ""
    if isinstance(value, (list, dict)):
        return len(value) > 0
    return True


def _loose_equal(left: Any, right: Any) -> bool:
    if left is None and right is None:
        return True
    if left is None or right is None:
        return False
    if isinstance(left, bool) or isinstance(right, bool):
        return _truthy(left) == _truthy(right)
    return str(left) == str(right)


def unwrap(text: str) -> str:
    stripped = text.strip()
    if stripped.startswith("${{") and stripped.endswith("}}"):
        return stripped[3:-2].strip()
    return stripped


def eval_expr(value: Any, ctx: dict[str, Any]) -> Any:
    """Evaluate a workflow value: one expression, or a `${{ a }}-${{ b }}` mix.

    A string that is exactly one expression keeps the expression's native type
    (that is how `github.event.pull_request.number || github.run_id` can be a
    number). Any surrounding literal text forces string concatenation, which
    is what Actions does for interpolation.
    """
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, list):
        return [eval_expr(item, ctx) for item in value]
    if not isinstance(value, str):
        return value
    stripped = value.strip()
    if "${{" not in stripped:
        return stripped
    if stripped.startswith("${{") and stripped.endswith("}}") and stripped.count("${{") == 1:
        return _Parser(_tokenize(unwrap(stripped)), ctx, stripped).parse()
    parts: list[str] = []
    rest = stripped
    while rest:
        start = rest.find("${{")
        if start < 0:
            parts.append(rest)
            break
        parts.append(rest[:start])
        end = rest.find("}}", start)
        if end < 0:
            raise UnsupportedExpression(f"unterminated expression in {value!r}")
        expr = rest[start + 3 : end].strip()
        result = _Parser(_tokenize(expr), ctx, expr).parse()
        parts.append("" if result is None else str(result).lower() if isinstance(result, bool) else str(result))
        rest = rest[end + 2 :]
    return "".join(parts)


def eval_condition(value: Any, ctx: dict[str, Any]) -> bool:
    if value is None:
        return True
    if isinstance(value, bool):
        return value
    return _truthy(eval_expr(value, ctx))


# --------------------------------------------------------------------------
# Workflow model
# --------------------------------------------------------------------------


def _norm(doc: dict[str, Any]) -> dict[str, Any]:
    """PyYAML resolves the bare `on:` key to boolean True; normalise it."""
    out = dict(doc or {})
    if True in out and "on" not in out:
        out["on"] = out.pop(True)
    return out


def load_workflows(directory: str = WORKFLOW_DIR) -> dict[str, dict[str, Any]]:
    if yaml is None:
        raise SystemExit("PyYAML is required: python3 -m pip install pyyaml")
    workflows: dict[str, dict[str, Any]] = {}
    for name in sorted(os.listdir(directory)):
        if not name.endswith((".yml", ".yaml")):
            continue
        with open(os.path.join(directory, name), "r", encoding="utf-8") as handle:
            workflows[name] = _norm(yaml.safe_load(handle))
    return workflows


def glob_match(pattern: str, path: str) -> bool:
    if pattern.endswith("/**"):
        return path.startswith(pattern[:-2])
    if "/**/" in pattern:
        head, _, tail = pattern.partition("/**/")
        if not path.startswith(head + "/"):
            return False
        return fnmatch.fnmatchcase(path[len(head) + 1 :], tail) or glob_match(
            pattern.replace("/**/", "/", 1), path
        )
    return fnmatch.fnmatchcase(path, pattern)


@dataclass
class Event:
    """One synthetic Actions event."""

    name: str  # push | pull_request | workflow_dispatch
    ref: str
    ref_name: str
    ref_type: str
    changed: list[str] = field(default_factory=list)
    pr_number: int | None = None
    fork: bool = False
    run_id: str = "1000"
    label: str = ""
    vars: dict[str, str] = field(default_factory=dict)
    actor_id: str = '248477380'
    triggering_actor: str = 'louis-0hrs'
    inputs: dict[str, Any] = field(default_factory=dict)


def _paths_ok(trigger: dict[str, Any], event: Event) -> bool:
    if not event.changed:
        return True
    paths = trigger.get("paths")
    ignore = trigger.get("paths-ignore")
    if paths and not any(
        glob_match(pattern, path) for pattern in paths for path in event.changed
    ):
        return False
    if ignore and all(
        any(glob_match(pattern, path) for pattern in ignore) for path in event.changed
    ):
        return False
    return True


def trigger_matches(on_block: Any, event: Event) -> bool:
    if isinstance(on_block, str):
        return on_block == event.name
    if isinstance(on_block, list):
        return event.name in on_block
    if isinstance(on_block, dict):
        if event.name not in on_block:
            return False
        trigger = on_block[event.name] or {}
        if not isinstance(trigger, dict):
            return True
        if event.name == "pull_request":
            types = trigger.get("types")
            if types and "opened" not in types and "synchronize" not in types:
                return False
        if event.name in ("push", "pull_request"):
            branches = trigger.get("branches")
            tags = trigger.get("tags")
            if event.name == "push" and tags and event.ref_type == "tag":
                if not any(glob_match(pattern, event.ref_name) for pattern in tags):
                    return False
            elif branches:
                if event.ref_type == "tag":
                    return False
                if not any(glob_match(pattern, event.ref_name) for pattern in branches):
                    return False
        return _paths_ok(trigger, event)
    raise UnsupportedExpression(f"unsupported `on:` block {on_block!r}")


def _matrix_rows(job: dict[str, Any], ctx: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    matrix = (job.get("strategy") or {}).get("matrix")
    if not matrix:
        return [{}]
    rows: list[dict[str, Any]] = []
    includes = matrix.get("include") or []
    if isinstance(includes, str):
        includes = eval_expr(includes, ctx or {})
    if not isinstance(includes, list):
        raise UnsupportedExpression("matrix.include must resolve to a list")
    base_keys = [
        key
        for key, value in matrix.items()
        if key != "include" and isinstance(value, list)
    ]
    if base_keys:
        combos: list[dict[str, Any]] = [{}]
        for key in base_keys:
            combos = [dict(combo, **{key: item}) for combo in combos for item in matrix[key]]
        rows.extend(combos)
    if includes:
        rows.extend(includes)
    return rows or [{}]


def build_ctx(workflow_name: str, event: Event) -> dict[str, Any]:
    head_repo = "someone/HyperCine" if event.fork else "louis-0hrs/HyperCine"
    return {
        "github": {
            "event_name": event.name,
            "ref": event.ref,
            "ref_name": event.ref_name,
            "ref_type": event.ref_type,
            "workflow": workflow_name,
            "run_id": event.run_id,
            "sha": "0" * 40,
            "actor_id": event.actor_id,
            "triggering_actor": event.triggering_actor,
            "repository": "louis-0hrs/HyperCine",
            "event": {
                "pull_request": {
                    "number": event.pr_number or 1,
                    "base": {"sha": "b" * 40, "ref": "main"},
                    "head": {
                        "sha": "h" * 40,
                        "ref": event.ref_name,
                        "repo": {"full_name": head_repo, "fork": event.fork},
                    },
                },
                "before": "c" * 40,
            },
        },
        "vars": dict(event.vars),
        "inputs": {"checkout_ref": "", **event.inputs},
        "matrix": {},
        "env": {},
        "secrets": {},
    }


@dataclass
class Scheduled:
    workflow: str
    job_id: str
    job_name: str
    labels: list[str]
    matrix_name: str
    group: str | None

    def describe(self) -> str:
        labels = ",".join(self.labels)
        suffix = f" [{self.matrix_name}]" if self.matrix_name else ""
        return f"{self.workflow}:{self.job_id}{suffix} -> {labels}"


def resolve_labels(value: Any, ctx: dict[str, Any]) -> list[str]:
    resolved = eval_expr(value, ctx)
    if isinstance(resolved, str):
        return [part for part in resolved.replace(",", " ").split() if part]
    if isinstance(resolved, list):
        labels: list[str] = []
        for item in resolved:
            labels.extend(resolve_labels(item, ctx) if item is not None else [])
        return labels
    raise UnsupportedExpression(f"unresolvable runs-on {value!r}")


def plan(docs: dict[str, dict[str, Any]], event: Event) -> list[Scheduled]:
    scheduled: list[Scheduled] = []
    for workflow_name, doc in docs.items():
        if not trigger_matches(doc.get("on"), event):
            continue
        concurrency = doc.get("concurrency")
        for job_id, job in (doc.get("jobs") or {}).items():
            for row in _matrix_rows(job, build_ctx(workflow_name, event)):
                ctx = build_ctx(workflow_name, event)
                ctx["matrix"] = row
                ctx["runner"] = _runner_ctx(row)
                if not eval_condition(job.get("if"), ctx):
                    continue
                labels = resolve_labels(job.get("runs-on"), ctx)
                group = None
                if isinstance(concurrency, dict) and concurrency.get("group"):
                    group = str(eval_expr(concurrency["group"], ctx))
                scheduled.append(
                    Scheduled(
                        workflow=workflow_name,
                        job_id=job_id,
                        job_name=str(job.get("name", job_id)),
                        labels=labels,
                        matrix_name=str(row.get("name", "")),
                        group=group,
                    )
                )
    return scheduled


def _runner_ctx(row: dict[str, Any]) -> dict[str, Any]:
    runner = str(row.get("system", ""))
    os_name = {"Darwin": "macOS", "Linux": "Linux", "Windows": "Windows"}.get(runner, runner)
    return {"os": os_name, "arch": str(row.get("machine", "")), "environment": "self-hosted"}


def _is_self_hosted(labels: Iterable[str]) -> bool:
    return any(label == "self-hosted" for label in labels)


def _hosted_label(labels: Iterable[str]) -> str | None:
    for label in labels:
        if label in HOSTED_OS_LABELS:
            return label
    return None


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------


def scenarios() -> list[Event]:
    src = ["src/core/pipeline.cpp"]
    standalone_entry = ["CMakeLists.txt", "src/core/pipeline.cpp"]
    return [
        Event(
            name="pull_request",
            ref="refs/pull/42/merge",
            ref_name="feature/x",
            ref_type="branch",
            changed=standalone_entry,
            pr_number=42,
            run_id="2001",
            label="pr with source changes",
        ),
        Event(
            name="pull_request",
            ref="refs/pull/42/merge",
            ref_name="feature/x",
            ref_type="branch",
            changed=["docs/notes.md"],
            pr_number=42,
            run_id="2002",
            label="pr with docs only",
        ),
        Event(
            name="push",
            ref="refs/heads/feature/x",
            ref_name="feature/x",
            ref_type="branch",
            changed=standalone_entry,
            pr_number=42,
            run_id="2003",
            label="feature branch push with open pr",
        ),
        Event(
            name="push",
            ref="refs/heads/main",
            ref_name="main",
            ref_type="branch",
            changed=standalone_entry,
            run_id="2004",
            label="main push",
        ),
        Event(
            name="push",
            ref="refs/tags/v1.0.0",
            ref_name="v1.0.0",
            ref_type="tag",
            changed=standalone_entry,
            run_id="2005",
            label="tag push",
        ),
        Event(
            name="workflow_dispatch",
            ref="refs/heads/main",
            ref_name="main",
            ref_type="branch",
            changed=src,
            run_id="2006",
            label="manual dispatch",
        ),
        Event(
            name="workflow_dispatch",
            ref="refs/heads/feature/x",
            ref_name="feature/x",
            ref_type="branch",
            changed=standalone_entry,
            run_id="2008",
            label="manual feature-ref dispatch",
        ),
        Event(
            name="pull_request",
            ref="refs/pull/7/merge",
            ref_name="contributor/patch",
            ref_type="branch",
            changed=standalone_entry,
            pr_number=7,
            fork=True,
            run_id="2007",
            label="fork pull request",
        ),
    ]


def _by(planned: list[Scheduled], workflow: str, job_id: str) -> list[Scheduled]:
    return [item for item in planned if item.workflow == workflow and item.job_id == job_id]


# --------------------------------------------------------------------------
# Rules
# --------------------------------------------------------------------------


@dataclass
class Finding:
    rule: str
    detail: str


def check_contract(docs: dict[str, dict[str, Any]]) -> list[Finding]:
    findings: list[Finding] = []
    events = scenarios()
    plans = {event.label: plan(docs, event) for event in events}

    def violation(rule: str, detail: str) -> None:
        findings.append(Finding(rule, detail))

    # R1/R2/R3: the required privacy check keeps its scope and stays hosted.
    for event in events:
        if event.name == "workflow_dispatch":
            continue
        hits = _by(plans[event.label], PRIVACY_WORKFLOW, PRIVACY_JOB)
        if event.name == "pull_request" or (
            event.name == "push" and event.ref_type == "branch"
        ):
            if not hits:
                violation(
                    "R1",
                    f"required privacy check not scheduled for {event.label}",
                )
                continue
            for hit in hits:
                if _is_self_hosted(hit.labels):
                    violation(
                        "R2",
                        f"required privacy check routed to self-hosted for {event.label}"
                        f" ({','.join(hit.labels)})",
                    )
                if not _hosted_label(hit.labels):
                    violation(
                        "R2",
                        f"required privacy check has no github-hosted label for {event.label}"
                        f" ({','.join(hit.labels) or 'empty'})",
                    )

    # R4/R5: self-hosted routing must be explicit and platform-consistent.
    expected_platform = {
        "hc-linux-x64-cpu": ("Linux",),
        "hc-linux-x64-cuda12.8": ("Linux",),
        "hc-windows-x64-msvc": ("Windows",),
        "hc-macos-arm64": ("macOS",),
    }
    for label, plan_items in plans.items():
        for item in plan_items:
            caps = [tag for tag in item.labels if tag.startswith(PLATFORM_PREFIX)]
            if not caps:
                continue
            if len(item.labels) != 2 or len(caps) != 1 or not _is_self_hosted(item.labels):
                violation(
                    "R4",
                    f"{label}: {item.describe()} must be exactly [self-hosted, <one hc-*> label]"
                    " -- a custom label alone is not runnable and would never report status",
                )
                continue
            if caps[0] not in CAPABILITY_LABELS:
                violation("R4", f"{label}: unknown capability label {caps[0]}")
                continue
            os_expected = expected_platform[caps[0]]
            name = item.matrix_name or item.job_id
            if not any(os_name in name or os_name in item.job_name for os_name in os_expected):
                violation(
                    "R5",
                    f"{label}: {caps[0]} assigned to a non-{os_expected[0]} lane ({name})",
                )

    # R6: fork pull requests never reach a self-hosted host.
    fork_items = plans["fork pull request"]
    if not fork_items:
        violation("R6", "fork pull request scheduled nothing at all")
    for item in fork_items:
        if _is_self_hosted(item.labels):
            violation(
                "R6",
                f"fork pull request executes {item.describe()} on self-hosted",
            )


    # R13: the public repository must NEVER request any self-hosted runner.
    # Restricting workflow events cannot stop another workflow stealing a
    # repository-scoped public runner. The private relay is the only VM path.
    enabled_plans = {}
    for event in events:
        enabled = replace(event, vars={**event.vars, "HYPERCINE_CI_SELF_HOSTED": "1",
                                       "HYPERCINE_CI_LOCAL_MAC": "1"})
        items = plan(docs, enabled)
        enabled_plans[event.label] = items
        for item in items:
            if _is_self_hosted(item.labels):
                violation("R13", f"public repository requests {item.describe()}")

    # R14: no automatic paid Mac job, regardless of old activation variables.
    # The only public Mac path is a fresh owner manual approval for exact SHA.
    key = lambda i: (i.workflow, i.job_id, i.matrix_name, tuple(i.labels))
    for event in events:
        baseline = plan(docs, event)
        actual = enabled_plans[event.label]
        if any(_hosted_label(i.labels) and _hosted_label(i.labels).startswith('macos') for i in actual):
            violation('R14', f'Automatic hosted Mac fallback for {event.label}')
        if sorted(map(key, actual)) != sorted(map(key, baseline)):
            violation('R14', f'Legacy activation flag changes non-Mac jobs for {event.label}')
    manual = Event('workflow_dispatch','refs/heads/main','main','branch',
                   inputs={'mac_hosted_fallback':True,'mac_candidate_sha':'0'*40})
    approved = plan(docs,manual)
    mac = [i for i in approved if i.labels == ['macos-15']]
    if len(mac) != 3:violation('R14','Explicit owner fallback must preserve exactly three Mac lanes')
    for denied in [replace(manual,actor_id='99'),replace(manual,triggering_actor='other'),
                   replace(manual,inputs={'mac_hosted_fallback':False,'mac_candidate_sha':'0'*40}),
                   replace(manual,inputs={'mac_hosted_fallback':True,'mac_candidate_sha':'1'*40})]:
        if any(i.labels == ['macos-15'] for i in plan(docs,denied)):
            violation('R14','Unapproved actor/rerun/SHA starts hosted Mac')

    # R7: a docs-only PR must not start the five-job decode matrix.
    docs_only = plans["pr with docs only"]
    if _by(docs_only, "ci-standalone.yml", "standalone"):
        violation("R7", "docs-only PR schedules the Standalone matrix")

    # R8: a source PR keeps all five Standalone lanes and their platform labels.
    src_pr = _by(plans["pr with source changes"], "ci-standalone.yml", "standalone")
    if len(src_pr) != 3:
        violation("R8", f"source PR schedules {len(src_pr)} Standalone rows, expected 3")
    else:
        names = sorted(item.matrix_name for item in src_pr)
        expected = sorted(
            [
                "Linux x64 Release",
                "Linux x64 ASan UBSan",
                "Windows MSVC x64 Release",
            ]
        )
        if names != expected:
            violation("R8", f"Standalone matrix rows changed: {names}")

    # R9: tag pushes never start the decode matrix or the CUDA compile gate.
    tag_items = plans["tag push"]
    for wf in ("ci-standalone.yml", "ci-cuda-build.yml"):
        if any(item.workflow == wf for item in tag_items):
            violation("R9", f"tag push schedules {wf}")

    # R10: each workflow's concurrency group is stable per (workflow, PR) and
    # distinct across PRs. Missing concurrency is reported, not failed: trigger
    # trimming is tracked separately in #350.
    first = plan(docs, events[0])
    second = plan(
        docs,
        Event(
            name="pull_request",
            ref="refs/pull/42/merge",
            ref_name="feature/x",
            ref_type="branch",
            changed=["src/core/pipeline.cpp"],
            pr_number=42,
            run_id="9999",
            label="pr second commit",
        ),
    )

    def groups(items: list[Scheduled]) -> dict[str, str]:
        return {item.workflow: item.group for item in items}

    g1, g2 = groups(first), groups(second)
    for workflow in sorted(set(g1) & set(g2)):
        if g1[workflow] is None:
            continue
        if g1[workflow] != g2[workflow]:
            violation(
                "R10",
                f"{workflow}: concurrency group differs between two commits of PR 42"
                f" ({g1[workflow]} vs {g2[workflow]})",
            )

    # R11: a self-hosted CUDA host must not rely on the hosted image's toolkit.
    cuda_plan = enabled_plans["main push"]
    for item in cuda_plan:
        if item.workflow != "ci-cuda-build.yml" or not _is_self_hosted(item.labels):
            continue
        job = docs["ci-cuda-build.yml"]["jobs"][item.job_id]
        names = {str(step.get("name", "")) for step in job.get("steps", [])}
        if "Verify pinned CUDA toolkit (self-hosted)" not in names:
            violation(
                "R11",
                f"self-hosted CUDA job {item.job_id} lacks an explicit pinned-toolkit verification step",
            )
        if "Install pinned CUDA toolkit and build dependencies" in names:
            installed = next(
                step
                for step in job["steps"]
                if step.get("name") == "Install pinned CUDA toolkit and build dependencies"
            )
            if "runner.environment" not in str(installed.get("if", "")):
                violation(
                    "R11",
                    f"self-hosted CUDA job {item.job_id} still installs the toolkit unconditionally",
                )

    return findings


def check_local_evidence(docs: dict[str, dict[str, Any]]) -> list[Finding]:
    """Policy checks that need no event: no bare labels, no secrets to PR code."""
    findings: list[Finding] = []
    for workflow_name, doc in docs.items():
        trigger = doc.get("on") if isinstance(doc.get("on"), dict) else {}
        uses_pr_target = "pull_request_target" in (trigger or {})
        for job_id, job in (doc.get("jobs") or {}).items():
            for row in _matrix_rows(job, build_ctx(workflow_name, scenarios()[0])):
                ctx = build_ctx(workflow_name, scenarios()[0])
                ctx["matrix"] = row
                labels = resolve_labels(job.get("runs-on"), ctx)
                if labels == ["self-hosted"]:
                    findings.append(
                        Finding(
                            "R12",
                            f"{workflow_name}:{job_id} uses the bare `self-hosted` label",
                        )
                    )
                if uses_pr_target and _is_self_hosted(labels):
                    findings.append(
                        Finding(
                            "R12",
                            f"{workflow_name}:{job_id} combines pull_request_target with self-hosted",
                        )
                    )
    return findings


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def render_table(docs: dict[str, dict[str, Any]], local_mac: bool = False) -> str:
    lines: list[str] = []
    for event in scenarios():
        if local_mac:
            event = replace(event, vars={"HYPERCINE_CI_LOCAL_MAC": "1"})
        lines.append(f"## {event.label} ({event.name} on {event.ref})")
        for item in plan(docs, event):
            lines.append(f"  {item.describe()}")
        lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workflow-dir", default=WORKFLOW_DIR)
    parser.add_argument("--list", action="store_true", help="print the resolved routing table")
    parser.add_argument("--json", action="store_true", help="machine-readable findings")
    parser.add_argument("--local-mac", action="store_true", help="resolve with Mac relay enabled")
    args = parser.parse_args(argv)

    docs = load_workflows(args.workflow_dir)

    if args.list:
        sys.stdout.write(render_table(docs, args.local_mac))
        return 0

    findings = check_contract(docs) + check_local_evidence(docs)
    if args.json:
        print(json.dumps([f.__dict__ for f in findings], indent=2))
    else:
        for finding in findings:
            print(f"VIOLATION {finding.rule}: {finding.detail}")
        if not findings:
            checked = sum(len(plan(docs, event)) for event in scenarios())
            print(
                f"OK: {len(docs)} workflows, {len(scenarios())} events, "
                f"{checked} job instances, 14 rules"
            )
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
