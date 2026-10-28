# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import copy
import json
import subprocess
import sys
import shutil
import tempfile
import unittest
from pathlib import Path
from verify_evidence import ROOT,ALL,OLD,audit,audit28,audit96,read,unique_object,manifest

class EvidenceTests(unittest.TestCase):
    def test_complete(self):
        self.assertEqual(audit()["calls"],124)

    def test_reject_all_channel_mutations(self):
        plan=read(ROOT/ALL/"plan.json")
        calls=read(ROOT/ALL/"calls.json")
        summaries=read(ROOT/ALL/"summary.json")
        for mutation in ["duplicate","wait","missing_publication","wrong_difference","wrong_arm","guard"]:
            with self.subTest(mutation=mutation):
                c,s=copy.deepcopy(calls),copy.deepcopy(summaries)
                if mutation=="duplicate":c[1]=copy.deepcopy(c[0])
                if mutation=="wait":c[0]["waits_ok"]=False
                if mutation=="guard":s[0]["guards_ok"]=False
                if mutation=="wrong_arm":c[0]["mask"]=7
                if mutation=="missing_publication":
                    for call in s:
                        for e in call["comparisons"]:
                            if e["phase"]==2:e["valid"]=0
                if mutation=="wrong_difference":
                    next(e for e in s[2]["comparisons"] if e["phase"]==0 and e["index"]==1 and e["ch"]==0)["first_difference"]["coefficient"]=1
                with self.assertRaises((ValueError,StopIteration)):
                    audit28(plan,c,s)

    def test_reject_legacy_mutations(self):
        p=read(ROOT/OLD/"plan.json");c=read(ROOT/OLD/"calls.json")
        s=read(ROOT/OLD/"summary.json");v=read(ROOT/OLD/"verification.json")
        for mutation in ["api","duplicate","totals","signal"]:
            with self.subTest(mutation=mutation):
                bad,summary=copy.deepcopy(c),copy.deepcopy(s)
                if mutation=="api":bad[0]["exit"]=1
                if mutation=="duplicate":bad[1]=copy.deepcopy(bad[0])
                if mutation=="totals":summary[0]["differing_outputs"]=8
                if mutation=="signal":next(x for x in bad if x["delay"]=="1")["wait_signalled"]=False
                with self.assertRaises(ValueError):audit96(p,bad,summary,v)

    def test_duplicate_json(self):
        with self.assertRaises(ValueError):
            json.loads('{"entry":1,"entry":2}',object_pairs_hook=unique_object)

    def test_manifest_and_inventory(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for folder in [ALL,OLD]:shutil.copytree(ROOT/folder,root/folder)
            shutil.copyfile(ROOT/"rca160-public-manifest.json",root/"rca160-public-manifest.json")
            path=root/ALL/"calls.json"
            original=path.read_bytes()
            path.write_bytes(original+b" ")
            with self.assertRaisesRegex(ValueError,"SHA-256"):manifest(root)
            path.write_bytes(original)
            extra=root/ALL/"unexpected.txt"
            extra.write_text("not in public inventory")
            with self.assertRaisesRegex(ValueError,"inventory"):manifest(root)
            extra.unlink()
            path.unlink()
            with self.assertRaisesRegex(ValueError,"inventory"):manifest(root)

    def test_optimized_interpreter(self):
        # Run the negative tests too: a green -O audit alone would not prove
        # that failed predicates survive optimization.
        result=subprocess.run([sys.executable,"-O",__file__,"--optimized-child"],
                              capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=="__main__":
    if "--optimized-child" in sys.argv:
        suite=unittest.TestSuite(EvidenceTests(name) for name in [
            "test_complete","test_reject_all_channel_mutations",
            "test_reject_legacy_mutations","test_duplicate_json","test_manifest_and_inventory"])
        result=unittest.TextTestRunner().run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    unittest.main()
