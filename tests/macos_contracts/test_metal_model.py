# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import ast
from pathlib import Path
import unittest
from unittest.mock import patch
import metal_budget

class ModelContract(unittest.TestCase):
    def test_acceptance_checks_survive_optimization(self):
        source = Path(metal_budget.__file__).read_text()
        self.assertFalse(any(isinstance(node, ast.Assert) for node in ast.walk(ast.parse(source))))

    def test_faulty_padding_accounting_fails(self):
        original=metal_budget.budget
        def corrupt(width,height,padding=0):
            row=original(width,height,padding)
            if padding:
                row['resident_bytes']+=1
            return row
        with patch.object(metal_budget,'budget',side_effect=corrupt):
            with self.assertRaisesRegex(ValueError,'padding accounting'):
                metal_budget.main()

    def test_padding_and_buffer_count(self):
        for w, h in [(1920,1080), (3840,2160), (7680,4320), (12288,8040), (17520,8040)]:
            row, padded = metal_budget.budget(w,h), metal_budget.budget(w,h,16)
            self.assertEqual(row['buffer_count'],42)
            self.assertEqual(padded['resident_bytes']-row['resident_bytes'],16*(h-1))

    def test_invalid_model_geometry_rejected(self):
        for width, height, padding in ((0,1080,0), (-1920,1080,0),
                (1921,1080,0), (1920,0,0), (1920,-1080,0),
                (True,1080,0), (1920,1080.0,0), (1920,1080,-1),
                (1920,1080,False), (1920,1080,0.5)):
            with self.subTest(width=width,height=height,padding=padding), self.assertRaises(ValueError):
                metal_budget.budget(width,height,padding)

if __name__ == '__main__':
    unittest.main()
