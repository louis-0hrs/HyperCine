# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Audit recorded JSON evidence; never invoke a decoder or require media."""
import hashlib
import json
from pathlib import Path
import re
from legacy_96 import verify as verify96
from all_channels_28 import verify as verify28

ROOT = Path(__file__).resolve().parents[2] / "docs/evidence"
OLD = "sdk-rca-2026-10-28"
ALL = "sdk-rca-all-channels-2026-10-28"
PACKET = "fc046231223e537bb221524b2d6f9c0d9f24a71ea23f79deb140f44f7bd063c8"

def require(condition, predicate):
    if not condition:
        raise ValueError(predicate)

def unique_object(items):
    out = {}
    for key, value in items:
        require(key not in out, "duplicate JSON key")
        out[key] = value
    return out

def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)

def digest(value):
    return isinstance(value,str) and re.fullmatch("[0-9a-f]{64}",value) is not None

def manifest(root):
    record = read(root / "rca160-public-manifest.json")
    actual = {p.relative_to(root).as_posix() for name in [OLD,ALL]
              for p in (root / name).iterdir()}
    require(actual == set(record["files"]), "evidence file inventory")
    for name, sha in record["files"].items():
        require(name in actual and ".." not in name and name.endswith(".json"), "manifest path")
        require(hashlib.sha256((root/name).read_bytes()).hexdigest() == sha, "file SHA-256: "+name)
    require(record["files"][ALL+"/summary.json"] ==
            "786a4d468f23754c2fc68d9002742f072fa3c6c547598e24835311052f84adbc", "historical summary hash")
    require(record["files"][ALL+"/verification.json"] ==
            "f68e73fcca8cb02adc00a05d8e94db364fcd29bd725b7ee9ba6cd700f778b843", "historical verdict hash")

def audit28(plan,calls,summaries):
    arms = [("serial",1,0,0),("induceY",8,1,1),("induceV",8,1,2),
            ("induceU",8,1,4),("induceAll",8,1,7),("publishAll",8,2,7),
            ("reinduceAll",8,1,7)]
    require(plan["packet_sha256"] == PACKET and plan["packet_size"] == 900396 and
            plan["repetitions"] == 2 and plan["arms"] == [list(a) for a in arms], "28-call plan")
    names = {f"{r}-{a}-{i}" for r in ["full","half"] for a,_,_,_ in arms for i in range(2)}
    require(len(calls)==len(summaries)==28 and
            {c["name"] for c in calls}=={s["name"] for s in summaries}==names, "28-call inventory")
    by_name = {s["name"]:s for s in summaries}
    for c in calls:
        s = by_name[c["name"]]
        res,arm,repeat = c["name"].split("-")
        spec = next(a for a in arms if a[0]==arm)
        require(c["resolution"]==res and (c["cpu"],c["mode"],c["mask"])==spec[1:], "arm identity")
        require(s["exit"]==0 and s["guards_ok"] and s["waits_ok"], "summary status")
        es = s["comparisons"]
        require(len(c["events"])==len(es) and [e["event"] for e in es]==list(range(len(es))), "event inventory")
        refs = by_name[res+"-serial-0"]["comparisons"]
        for original,e in zip(c["events"],es):
            require(all(e[k]==v for k,v in original.items()), "call/summary join")
            ref = next(r for r in refs if (r["ch"],r["phase"],r["index"])==(e["ch"],e["phase"],e["index"]))
            require(digest(e["sha256"]) and (e["w"],e["h"])==(ref["w"],ref["h"]), "snapshot identity")
            d = e["first_difference"]
            require((d is None)==(e["sha256"]==ref["sha256"]), "snapshot difference/hash")
            if d is not None:
                require(0<=d["x"]<e["w"] and 0<=d["y"]<e["h"] and
                        d["coefficient"]==d["y"]*e["w"]+d["x"] and
                        d["byte_offset"]==d["coefficient"]*2 and
                        -32768<=d["actual"]<=32767 and -32768<=d["expected"]<=32767 and
                        d["actual"]!=d["expected"], "row-major signed coefficient")
            if c["mode"]==1 and c["mask"] & (1<<e["ch"]) and e["index"]==1 and e["phase"] in [0,1]:
                expected = [2700,1063,709] if e["phase"]==0 else [2696,1066,708]
                actual = [0,0,0] if e["phase"]==0 else [-10,6,0]
                require(d is not None and d["coefficient"]==0 and
                        (d["expected"],d["actual"])==(expected[e["ch"]],actual[e["ch"]]),
                        "reported first coefficient value")
        require(digest(s["pixel_sha256"]) and s["pixel_equal"] ==
                (s["pixel_sha256"]==by_name[res+"-serial-0"]["pixel_sha256"]), "pixel equality/hash")
    # Historical predicates, paired by name instead of trusting array order.
    return verify28(calls,[by_name[c["name"]] for c in calls])

def audit96(plan,calls,summaries,verification):
    require(plan["packet_sha256"]==PACKET and plan["packet_size"]==900396, "96-call packet")
    arms=[("stock","1","0"),("stock","8","0"),("probe","1","0"),
          ("probe","8","0"),("probe","8","1"),("probe","8","2")]
    require(plan["arms"]==[list(a) for a in arms], "96-call plan")
    names={f"{r}-{k}-{w}-{m}-{i}" for r in ["full","half"] for k,w,m in arms for i in range(8)}
    require(len(calls)==96 and {c["name"] for c in calls}==names and len(summaries)==12, "96-call inventory")
    keys={(s["resolution"],s["kind"],s["workers"],s["delay"]) for s in summaries}
    require(len(keys)==12, "unique 96-call summaries")
    for s in summaries:
        group=[c for c in calls if (c["resolution"],c["kind"],c["workers"],c["delay"]) ==
               (s["resolution"],s["kind"],s["workers"],s["delay"])]
        require(len(group)==8 and all(digest(c["sha256"]) for c in group), "96-call group")
        require(all(c["name"].startswith(f'{c["resolution"]}-{c["kind"]}-{c["workers"]}-{c["delay"]}-')
                    and c["fill"] in [0,165] for c in group), "96-call identity")
        serial=next(c["sha256"] for c in calls if (c["resolution"],c["kind"],c["workers"]) ==
                    (s["resolution"],"stock","1"))
        require(s["calls"]==8 and s["errors"]==0 and s["control_stable"] and
                s["differing_outputs"]==sum(c["sha256"]!=serial for c in group) and
                s["late_producer"]==sum(bool(c["late_producer_channels"]) for c in group) and
                s["signalled_waits"]==sum(c["wait_signalled"] for c in group), "recomputed 96-call totals")
        require(len(s["differences"])==8 and {d["name"] for d in s["differences"]} ==
                {c["name"] for c in group}, "pixel-difference inventory")
    result=verify96(plan,calls)
    require(result==verification["results"], "historical 96-call verifier output")
    return result

def audit(root=ROOT):
    manifest(root)
    get=lambda folder,name: read(root/folder/(name+".json"))
    old=audit96(get(OLD,"plan"),get(OLD,"calls"),get(OLD,"summary"),get(OLD,"verification"))
    verdict=audit28(get(ALL,"plan"),get(ALL,"calls"),get(ALL,"summary"))
    historical=get(ALL,"verification")
    require(historical["verdict"]==verdict and historical["calls"]==28 and
            historical["negative_controls_rejected"]==4, "historical all-channel result")
    return dict(status="RECORDED_LEDGER_AUDIT_PASS",calls=124,historical_verdict=verdict,
                legacy_channel1_results=old,decoder_replayed=False,
                raw_coefficient_differences_recomputed=False,original_sdk_fixed=False)

if __name__=="__main__":
    try:
        print(json.dumps(audit(),indent=2))
    except (ValueError,KeyError,TypeError,StopIteration,OSError) as error:
        raise SystemExit("RECORDED_LEDGER_AUDIT_FAIL: "+str(error))
