"""Optional acceleration; explicit Python mode remains the reference."""

import os
from . import metrics


def select():
    name = os.environ.get("RETOQUE_REPLAY_BACKEND", "auto")
    if name not in ("auto", "python", "numpy"):
        raise ValueError("RETOQUE_REPLAY_BACKEND must be auto, python or numpy")
    if name != "python":
        try:
            from . import numpy_metrics
        except ImportError:
            if name == "numpy":
                raise ValueError("NumPy unavailable; use uv run --with numpy") from None
        else:
            return "numpy", numpy_metrics
    return "python", metrics
