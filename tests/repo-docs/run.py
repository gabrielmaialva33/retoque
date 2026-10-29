#!/usr/bin/env python3
"""Real filesystem fixtures for documentation destinations and catalog drift."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/check_docs.py'
checker = None
if TOOL.exists():
    spec = importlib.util.spec_from_file_location('retoque_docs_under_test', TOOL)
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)


class Documentation(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(checker, 'documentation checker is not implemented')
        temporary = tempfile.TemporaryDirectory(prefix='retoque-docs-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root/'docs').mkdir()
        self.doc = self.root/'docs/guide.md'

    def check(self, text):
        self.doc.write_text(text)
        return checker.missing_targets(self.root, [self.doc])

    def test_relative_markdown_and_html_targets_are_checked(self):
        (self.root/'ok.txt').write_text('ok')
        problems = self.check('[ok](../ok.txt)\n[missing](absent.md)\n<img src="lost.svg">\n')
        self.assertEqual(len(problems), 2)
        self.assertIn('absent.md', problems[0])
        self.assertIn('lost.svg', problems[1])

    def test_fences_code_remote_urls_and_anchors_are_not_fetched(self):
        self.assertEqual(self.check('```md\n[fake](missing.md)\n```\n'
                                   '`[example](missing.md)`\n[web](https://example.invalid/file)\n'
                                   '[anchor](#heading)\n![data](data:image/png;base64,abc)\n'), [])

    def test_reference_links_spaces_and_root_relative_targets(self):
        (self.root/'a b.md').write_text('# Hello')
        self.assertEqual(self.check('[a](../a%20b.md#hello)\n[x]: </a b.md>\n'), [])
        self.assertEqual(len(self.check('[missing]: nowhere.md\n')), 1)

    def test_catalog_rejects_stale_content(self):
        path = self.root/'catalog.md'
        path.write_text('old\n')
        self.assertTrue(checker.catalog_problems(path, 'new\n'))
        path.write_text('new\n')
        self.assertEqual(checker.catalog_problems(path, 'new\n'), [])

    def test_catalog_missing_is_not_success(self):
        self.assertTrue(checker.catalog_problems(self.root/'missing.md', 'expected\n'))

    def test_checked_file_list_includes_untracked_docs_but_not_ignored_artifacts(self):
        import subprocess
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        (self.root/'.gitignore').write_text('artifacts/\n')
        (self.root/'README.md').write_text('# Read me')
        (self.root/'docs/new.md').write_text('# New')
        (self.root/'artifacts').mkdir()
        (self.root/'artifacts/log.md').write_text('[fake](missing)')
        names = {p.relative_to(self.root).as_posix() for p in checker.markdown_files(self.root)}
        self.assertIn('docs/new.md', names)
        self.assertIn('README.md', names)
        self.assertNotIn('artifacts/log.md', names)


if __name__ == '__main__':
    unittest.main()
