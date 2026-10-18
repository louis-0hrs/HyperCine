#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Recheck source/arithmetic assertions of PR #137; no GPU execution."""
import json
import subprocess

SOURCE = '6af34d01e42d68717c00482d079d67b4ac016afd'
DOC = 'docs/validation/macos-five-review/'


def git(*args):
    return subprocess.check_output(['git', *args], text=True)


def workspace_bytes(width, height, padding=0):
    def plane(w, h):
        return ((w + 7) // 8 * 8) * h * 2
    return ((width * 4 + padding) * (height - 1) + width * 4
            + sum(plane(cw, height) + sum(4 * plane(cw // 2**level, height // 2**level)
                  for level in (1, 2, 3)) for cw in (width, width // 2, width // 2))
            + 2 * plane(width // 2, height))


def main():
    report = json.loads(git('show', SOURCE + ':' + DOC + 'report.json'))
    readme = git('show', SOURCE + ':' + DOC + 'README.md')
    conditions = [report[field] is False for field in
                  ('parent_features_complete', 'private_sdk_corpus_configured',
                   'metal_kernel_execution', 'upstream_modified')]
    for review in report['reviews']:
        subprocess.run(['git', 'cat-file', '-e', review['source'] + '^{commit}'], check=True)
        conditions.extend((review['source'] in readme, '#' + str(review['issue']) in readme,
                           '#' + str(review['pr']) in readme))
    changed = git('diff', '--name-only', report['review_base'], SOURCE).splitlines()
    conditions.append(sorted(changed) == [DOC + 'README.md', DOC + 'report.json'])
    for (w, h), total in zip(((1920, 1080), (3840, 2160), (7680, 4320),
                             (12288, 8040), (17520, 8040)),
                            (31622400, 126489600, 505958400, 1506631680, 2148545280)):
        conditions.extend((workspace_bytes(w, h) == total, format(total, ',') in readme))
    metal = next(item for item in report['reviews'] if item['issue'] == 45)
    old, new = workspace_bytes(1920, 1080), workspace_bytes(1920, 1080, 16)
    conditions.extend((old + new == metal['reachable_1080p_pitch_replacement_bytes'],
                       new == metal['reported_new_workspace_bytes'], new - old == 16 * 1079))
    if not all(conditions):
        raise ValueError('report consistency failure')
    print('PASS report source/arithmetic consistency:', len(conditions), 'checks')


if __name__ == '__main__':
    main()
