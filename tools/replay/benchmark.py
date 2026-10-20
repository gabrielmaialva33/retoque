"""CPU metric microbenchmark, no GPU/model; python -m tools.replay.benchmark."""

import argparse
from array import array
import json
import platform
import statistics
import time
from .format import Image
from . import metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if min(args.width, args.height, args.repeats) < 1:
        parser.error("positive dimensions/repeats required")
    w, h = args.width, args.height
    source = Image(
        w, h, 3, array("f", (0.2 + (i % 97) * 0.001 for i in range(w * h * 3)))
    )
    output = Image(w, h, 3, array("f", (v * 1.1 for v in source.data)))
    reference = Image(w, h, 3, array("f", (v * 1.09 for v in source.data)))
    depth = Image(w, h, 1, array("f", [0.5]) * (w * h))
    motion = Image(w, h, 2, array("f", [-0.5, 0.25]) * (w * h))
    capture = dict(
        regions={"centre": [w // 4, h // 4, max(1, w // 2), max(1, h // 2)]},
        color_jitter="removed",
        _pixel_tolerance=0.01,
        depth=dict(kind="device", threshold=0.02),
        motion=dict(units="pixels", scale=[1, 1], includes_jitter=False),
    )
    frame = dict(time_ms=16, jitter=[0, 0], reset=False)
    backends = {"python": metrics}
    try:
        from . import numpy_metrics

        backends["numpy"] = numpy_metrics
    except ImportError:
        pass
    results = {}
    for name, backend in backends.items():
        previous = backend.retain(dict(frame, time_ms=0), source, depth, output)
        cases = {}
        for paired in (False, True):
            timings = []
            # First call warms imports/caches and is not included.
            for i in range(args.repeats + 1):
                start = time.perf_counter()
                backend.frame_metrics(
                    capture,
                    frame,
                    source,
                    depth,
                    motion,
                    output,
                    reference,
                    previous if paired else None,
                )
                elapsed = time.perf_counter() - start
                if i:
                    timings.append(elapsed)
            median = statistics.median(timings)
            cases["paired" if paired else "unpaired"] = dict(
                seconds=timings,
                median_seconds=median,
                extrapolated_3440x1440_seconds=median * (3440 * 1440) / (w * h),
                extrapolated_300_frames_minutes=median
                * (3440 * 1440)
                / (w * h)
                * 300
                / 60,
            )
        results[name] = cases
    print(
        json.dumps(
            dict(
                width=w,
                height=h,
                repeats=args.repeats,
                python=platform.python_version(),
                numpy=numpy_metrics.np.__version__ if "numpy" in backends else None,
                scope="frame_metrics only; excludes decoding, validation and PNG/I/O",
                results=results,
            ),
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
