#!/usr/bin/env python3
"""Deprecated shim → nevod_hub.py (Nevod Hub). Remove after one release."""
from __future__ import annotations

import sys
import warnings

warnings.warn(
    "mock_cloud_diy.py is deprecated; use nevod_hub.py (Nevod Hub).",
    DeprecationWarning,
    stacklevel=1,
)

from nevod_hub import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
