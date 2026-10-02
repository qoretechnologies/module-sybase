#!/usr/bin/python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check rejection of ambiguous artifacts and incomplete offline coverage."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

loader = importlib.util.spec_from_file_location('fixture', Path(__file__).with_name('run-tests.py'))
fixture = importlib.util.module_from_spec(loader)
loader.loader.exec_module(fixture)


class FixtureTests(unittest.TestCase):
    def test_exact_artifact_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, 'exactly one'):
                fixture.select_module([root])
            module = root / 'freetds-api-2.0.qmod'
            module.touch()
            (root / 'sybase-api-2.0.qmod').touch()
            self.assertEqual(module, fixture.select_module([root]))
            (root / 'freetds-api-1.5.qmod').touch()
            with self.assertRaisesRegex(RuntimeError, 'exactly one'):
                fixture.select_module([root])

    def test_complete_coverage(self):
        fixture.verify_suite('Ran 4 test cases, 4 succeeded (14 assertions)\n')

    def test_incomplete_coverage(self):
        for output in ('', 'Ran 4 test cases, 3 succeeded (14 assertions)',
                       'Ran 4 test cases, 4 succeeded (13 assertions)'):
            with self.subTest(output=output), self.assertRaisesRegex(RuntimeError, 'Incomplete'):
                fixture.verify_suite(output)

    def test_diagnostics_fail(self):
        for diagnostic in ('Skipped: no client', 'warning: wrong client'):
            with self.subTest(diagnostic=diagnostic), self.assertRaisesRegex(RuntimeError, 'Unexpected'):
                fixture.verify_suite(diagnostic + '\nRan 4 test cases, 4 succeeded (14 assertions)\n')


if __name__ == '__main__':
    unittest.main()
