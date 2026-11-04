# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Local review CLI. Preview is read-only; execution requires an explicit flag."""
import argparse,json,os
from pathlib import Path
from urllib.parse import quote
from . import controller
from .adapters import GitHubCLI,KeyReader

def main(argv=None,api_factory=GitHubCLI):
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config',type=Path,required=True)
    p.add_argument('--state',type=Path)
    action=p.add_mutually_exclusive_group(required=True)
    action.add_argument('--preview',type=int,metavar='PR')
    action.add_argument('--approve',type=int,metavar='PR')
    action.add_argument('--cycle',action='store_true')
    p.add_argument('--head');p.add_argument('--base')
    p.add_argument('--allow-draft',action='store_true',help='Explicit advisory-only draft permission')
    p.add_argument('--execute',action='store_true',help='Required for a real controller cycle')
    a=p.parse_args(argv)
    cfg=json.loads(a.config.read_text());cfg=cfg.get('premerge',cfg)
    if a.cycle and not a.execute:p.error('--cycle requires explicit --execute')
    if (a.cycle or a.approve) and a.state is None:p.error('Mutating local operations require --state')
    api=api_factory(cfg)
    if a.preview:
        pr=api(f'repos/{controller.SOURCE}/pulls/{a.preview}')
        base=api(f'repos/{controller.SOURCE}/branches/main')['commit']['sha']
        if pr.get('head',{}).get('repo',{}).get('full_name')!=controller.SOURCE:
            raise RuntimeError('Foreign-repository PR is not eligible')
        head=api(f'repos/{controller.SOURCE}/git/ref/heads/{quote(pr["head"]["ref"],safe="/")}')['object']['sha']
        result={'repository':controller.SOURCE,'pull_number':a.preview,'head_sha':head,'base_sha':base,
                'draft':pr.get('draft'),'controller_enabled':cfg.get('enabled',False),
                'advisory':not cfg.get('required_gate',False),'mutation_performed':False}
        try:
            result['candidate']=controller.capture(api,cfg,a.preview,head,base,allow_draft=a.allow_draft)
            result['can_approve']=True
        except RuntimeError as exc:result['can_approve']=False;result['reason']=str(exc)
        print(json.dumps(result,indent=2));return result
    if a.approve:
        if not a.head or not a.base:p.error('--approve requires explicit --head and --base')
        a.state.mkdir(parents=True,exist_ok=True)
        record=controller.approve(api,cfg,a.state,a.approve,a.head,a.base,allow_draft=a.allow_draft)
        print(json.dumps({'approved':record['approval'],'dispatched':False},indent=2));return record
    reader_cfg=cfg.get('key_reader',{})
    reader=KeyReader(reader_cfg.get('program'),reader_cfg.get('sha256'))
    factory=lambda reporter_cfg:controller.ProductionReporter(reporter_cfg,key_provider=reader)
    return controller.cycle(api,cfg,a.state,reporter_factory=factory)

if __name__=='__main__':
    try:main()
    except (RuntimeError,ValueError) as exc:raise SystemExit(str(exc)) from None
    except OSError:raise SystemExit('Local configuration/state unavailable; details suppressed') from None
