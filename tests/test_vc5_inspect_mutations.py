# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Backward mutations never modify the live source/worktree."""
import importlib.util
import io
from pathlib import Path
import types
import unittest

path = Path(__file__).with_name('test_vc5_inspect.py')
spec = importlib.util.spec_from_file_location('contract', path)
t = importlib.util.module_from_spec(spec)
spec.loader.exec_module(t)

MUTATIONS = [
 ('blanket_failure', "'status': status", "'status': 'FAIL'", 'test_refusal_classification'),
 ('default_argparse_echo', 'SafeArgumentParser(prog=', 'argparse.ArgumentParser(prog=', 'test_sanitized_parser_in_process'),
 ('lost_known_dialect', "'dialect': dialect", "'dialect': 'unknown'", 'test_known_dialect_failure'),
 ('bounded_read', 'stream.read(limit + 1)', 'stream.read()', 'test_bounded_read_request'),
 ('image_format', "require(tag != 84 or 1 <= value <= 4, 'IMAGE_FORMAT_RANGE')", 'pass', 'test_header_legality'),
 ('reference_state', "if d['state'] != 'completed':", 'if False:', 'test_reference_missing'),
 ('qualification', "elif not d['qualified']:", 'elif False:', 'test_warning_error_success_false_pass'),
 ('regular_file', "require(stat.S_ISREG(os.fstat(stream.fileno()).st_mode), 'REGULAR_FILE_REQUIRED')", 'pass', 'test_nonregular_descriptor_rejected_before_read'),

 ('framing', "if data[:4] != b'VC-5':", 'if False:', 'test_framing'),
 ('truncation', "require(len(data) % 4 == 0, 'TRUNCATED_SEGMENT')", 'pass', 'test_framing'),
 ('required', "require(REQUIRED <= fields.keys(), 'MISSING_HEADER')", 'pass', 'test_header_completeness'),
 ('body', "require(chunks > 0, 'MISSING_BODY')", 'pass', 'test_header_completeness'),
 ('header_tags', "require(tag in BASE or (3 in parts and tag in IMAGE), 'HEADER_TAG')", 'pass', 'test_header_legality'),
 ('duplicate', "require(tag not in fields, 'DUPLICATE_HEADER')", 'pass', 'test_header_legality'),
 ('positive', "require(tag not in POSITIVE or value > 0, 'HEADER_RANGE')", 'pass', 'test_header_legality'),
 ('precision', "require(tag not in (101, 102, 35) or value <= 16, 'PRECISION_RANGE')", 'pass', 'test_header_legality'),
 ('channel', "require(tag != 12 or value <= 4, 'CHANNEL_RANGE')", 'pass', 'test_header_legality'),
 ('channel_state', "require(tag != 62 or (12 in fields and value < fields[12]), 'CHANNEL_STATE')", 'pass', 'test_state_bounds'),
 ('subband_state', "require(tag != 48 or (14 in fields and value < fields[14]), 'SUBBAND_STATE')", 'pass', 'test_state_bounds'),
 ('bounds', "require(end <= len(data), 'CHUNK_BOUNDS')", 'pass', 'test_chunk_bounds'),
 ('reserved', "require(tag >> 8 == 0x60, 'RESERVED_LARGE_CHUNK')", 'pass', 'test_chunk_bounds'),
 ('body_state', "require(header, 'BODY_TAG_UNSUPPORTED')", 'pass', 'test_chunk_bounds'),
 ('pattern_context', 'tag in IMAGE', '(tag in IMAGE and tag != 106)', 'test_pattern_width_context'),
 ('prescale', '(14, 12, 10)', '(0, 4, 8)', 'test_prescale_dual_unresolved'),
 ('dialect', "if mismatch and syntax['status'] != 'FAIL':", 'if False:', 'test_legacy_claim'),
 ('parts', 'if missing:', 'if False:', 'test_missing_enabled_parts'),
 ('materials', "if not d['materials_available'] or not d['inventory_complete']:", 'if False:', 'test_reference_missing'),
 ('warnings', "d['warnings'] or d['errors']", "False or d['errors']", 'test_warning_error_success_false_pass'),
 ('errors', "d['warnings'] or d['errors']", "d['warnings'] or False", 'test_warning_error_success_false_pass'),
 ('exit', "or d['exit_code'] != 0", 'or False', 'test_warning_error_success_false_pass'),
 ('pixels', "elif not d['pixels_equal']:", 'elif False:', 'test_warning_error_success_false_pass'),
 ('input_budget', "require(type(data) is bytes and len(data) <= MAX_BYTES, 'INPUT_BUDGET')", 'pass', 'test_budgets'),
 ('segment_budget', "require(segments <= MAX_SEGMENTS, 'SEGMENT_BUDGET')", 'pass', 'test_budgets'),
 ('output_budget', "require(len(json.dumps(report)) <= MAX_OUTPUT, 'OUTPUT_BUDGET')", 'pass', 'test_budgets'),
 ('boolean', 'type(value) is int', 'isinstance(value, int)', 'test_strict_context_types'),
 ('coverage_false_pass', "row('coverage', 'UNQUALIFIED'", "row('coverage', 'PASS'", 'test_coverage_and_claims'),
]


class MutationTests(unittest.TestCase):
    def test_backward_mutations(self):
        original = t.v
        source = t.TOOL.read_text()
        try:
            for name, before, after, test in MUTATIONS:
                with self.subTest(mutation=name):
                    self.assertIn(before, source)
                    module = types.ModuleType('mutant')
                    module.__file__ = str(t.TOOL)
                    exec(compile(source.replace(before, after), '<in-memory-mutant>', 'exec'), module.__dict__)
                    t.v = module
                    result = unittest.TextTestRunner(stream=io.StringIO()).run(t.InspectorTests(test))
                    self.assertFalse(result.wasSuccessful(), name+' survived')
                    print('CAUGHT '+name)
        finally:
            t.v = original


if __name__ == '__main__':
    unittest.main()
