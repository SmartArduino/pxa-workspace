"""Locate the workspace pxadb client without requiring an install.

The GUI deliberately imports pxadb as a library instead of copying protocol
code, so both tools always speak exactly the same protocol.
"""

from __future__ import annotations

import pathlib
import sys


def load_pxadb():
    try:
        import pxadb  # type: ignore[import-not-found]

        return pxadb
    except ImportError:
        pass
    # tools/pxadb-gui/pxadb_gui/library.py -> tools/pxadb
    candidate = pathlib.Path(__file__).resolve().parents[2] / "pxadb"
    if (candidate / "pxadb.py").is_file():
        sys.path.insert(0, str(candidate))
        import pxadb  # type: ignore[import-not-found]

        return pxadb
    raise ImportError(
        "pxadb is unavailable; install it with: python3 -m pip install -e tools/pxadb"
    )


pxadb = load_pxadb()
