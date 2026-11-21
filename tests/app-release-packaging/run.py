#!/usr/bin/env python3
"""Windows app release packaging: the app archive (make_app_release.py) and the setup (make_setup.py)."""
from pathlib import Path
import sys
import unittest

SCRIPTS = Path(__file__).resolve().parents[2] / "tools/app/scripts"

if __name__ == "__main__":
    sys.path.insert(0, str(SCRIPTS))
    suite = unittest.defaultTestLoader.discover(str(SCRIPTS), pattern="test_make_*.py", top_level_dir=str(SCRIPTS))
    if suite.countTestCases() == 0:
        sys.exit("no packaging tests found in " + str(SCRIPTS))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
