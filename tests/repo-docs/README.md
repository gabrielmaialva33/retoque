# Documentation checks

Run `python3 tests/repo-docs/run.py`. Stdlib-only fixtures exercise local Markdown and HTML destinations, encoded spaces, reference definitions, code fences, ignored artifacts and generated catalog drift. No network, GPU, anchors or external-link availability is checked. Production entry: `python3 tools/check_docs.py`.
