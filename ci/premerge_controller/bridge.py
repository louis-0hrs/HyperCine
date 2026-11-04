# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Portable host interface, independent of machine paths or VM internals."""
import re

def sha(value):
    return isinstance(value,str) and re.fullmatch('[0-9a-f]{40}',value) is not None

def verify_private(api,cfg):
    repository=cfg.get('repository')
    if (not isinstance(repository,str) or not re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]*/[A-Za-z0-9_-][A-Za-z0-9_.-]*',repository)
        or repository.lower()==cfg['source'].lower() or not sha(cfg.get('relay_commit'))):
        raise RuntimeError('Reviewed private queue and workflow pin are not configured')
    meta=api('repos/'+repository)
    if meta.get('private') is not True or meta.get('default_branch')!='main':
        raise RuntimeError('Queue must remain private with default branch main')
    current=api('repos/'+repository+'/git/ref/heads/main')
    if current.get('object',{}).get('sha')!=cfg['relay_commit']:
        raise RuntimeError('Private workflow definition changed; local review required')
