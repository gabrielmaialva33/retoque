#!/usr/bin/env python3
"""Check local documentation destinations and the generated test catalog (no network)."""
from __future__ import annotations

import argparse
import html
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tomllib
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = ('external/', 'OptiScaler/', 'tools/local/')


def markdown_files(root: Path) -> list[Path]:
    listing = subprocess.run(['git', '-C', str(root), 'ls-files', '--cached', '--others',
                              '--exclude-standard', '-z'], check=True, capture_output=True)
    return [root / name for name in sorted(set(listing.stdout.decode().split('\0')))
            if name.endswith('.md') and not name.startswith(EXCLUDED) and (root / name).is_file()]


def local_targets(text: str):
    """Recognize ordinary inline/reference Markdown links and HTML src/href attributes.

    This deliberately is not a full CommonMark renderer or a heading-anchor validator.
    """
    fence = None
    for number, original in enumerate(text.splitlines(), 1):
        marker = re.match(r'^\s{0,3}(`{3,}|~{3,})', original)
        if marker:
            run = marker.group(1)
            if fence is None:
                fence = run
            elif run[0] == fence[0] and len(run) >= len(fence):
                fence = None
            continue
        if fence is not None:
            continue
        line = re.sub(r'(`+).*?\1', '', original)
        targets = re.findall(r'!?\[[^\]]*\]\(\s*(<[^>]+>|[^\s)]+)', line)
        reference = re.match(r'^\s*\[[^\]]+\]:\s*(<[^>]+>|\S+)', line)
        if reference:
            targets.append(reference.group(1))
        targets.extend(match[1] for match in re.findall(r'\b(?:src|href)\s*=\s*([\'"])(.*?)\1', line))
        for target in targets:
            parsed = urlsplit(html.unescape(target.strip('<>')))
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            yield number, unquote(parsed.path)


def missing_targets(root: Path, paths: list[Path]) -> list[str]:
    problems = []
    for source in paths:
        for number, target in local_targets(source.read_text(encoding='utf-8-sig')):
            destination = root / target.lstrip('/') if target.startswith('/') else source.parent / target
            if not destination.exists():
                problems.append(f'{source.relative_to(root)}:{number}: missing local destination: {target}')
    return problems


def catalog_text(root: Path) -> str:
    spec = importlib.util.spec_from_file_location('_retoque_catalog_runner', root / 'tests/run_all.py')
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    with (root / 'tests/suites.toml').open('rb') as handle:
        manifest = tomllib.load(handle)
    problems = runner.check_registry(manifest)
    if problems:
        raise ValueError('; '.join(problems))
    return runner.registry_markdown(manifest, sorted(manifest), 'domain')


def catalog_problems(path: Path, expected: str) -> list[str]:
    if not path.is_file() or path.read_text(encoding='utf-8') != expected:
        return ['tests/CATALOG.md is missing or stale; run python3 tools/check_docs.py --write-catalog']
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write-catalog', action='store_true', help='regenerate only tests/CATALOG.md')
    args = parser.parse_args()
    try:
        expected = catalog_text(ROOT)
        catalog = ROOT / 'tests/CATALOG.md'
        if args.write_catalog:
            catalog.write_text(expected, encoding='utf-8', newline='\n')
        paths = markdown_files(ROOT)
        problems = catalog_problems(catalog, expected) + missing_targets(ROOT, paths)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1
    for problem in problems:
        print(f'FAIL: {problem}', file=sys.stderr)
    if problems:
        return 1
    print(f'PASS: local destinations in {len(paths)} Markdown files and test catalog; '
          'anchors and external URLs not checked')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
