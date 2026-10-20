"""Re-describe compatible v1 planes as v2 without changing their bytes; test-only evidence utility."""

import copy


def plane_v2(desc):
    if desc.get("encoding") != "raw":
        raise ValueError("conformance requires raw v1 planes")
    d = copy.deepcopy(desc)
    fmt = {
        ("f16", 4): 10,
        ("f32", 4): 2,
        ("f32", 1): 41,
        ("f16", 2): 34,
        ("f32", 2): 16,
    }[(d.pop("dtype"), d.pop("channels"))]
    from .dxgi import FORMATS

    d.update(encoding="raw-dxgi", dxgi_format=fmt, pixel_bytes=FORMATS[fmt][0])
    return d


def capture_v2(c):
    if c["motion"]["units"] != "pixels" or c["motion"]["scale"] != [1, 1]:
        raise ValueError("conformance requires unit pixel motion")
    c = copy.deepcopy(c)
    c.update(version=2, finalized=True)
    c["motion"]["scale"] = "per-frame"
    for f in c["frames"]:
        w, h = f["color"]["width"], f["color"]["height"]
        f["params"].update(
            render_subrect=[w, h],
            depth_base=[0, 0],
            motion_base=[0, 0],
            ray_reconstruction=False,
        )
        f["params"].setdefault("create_flags", 1)
        f["params"].setdefault("mv_scale", [1, 1])
        f["params"].setdefault("frame_time_ms", 16)
        for k in ("color", "depth", "motion", "motion_oracle"):
            if k in f:
                f[k] = plane_v2(f[k])
    return c


def run_v2(r, capture_sha):
    r = copy.deepcopy(r)
    r.update(version=2, capture_sha256=capture_sha)
    for f in r["frames"]:
        f["output"] = plane_v2(f["output"])
    return r


def projection(report):
    return {
        k: report.get(k)
        for k in (
            "verdict",
            "reasons",
            "metrics",
            "frames",
            "counts",
            "timing",
            "pulse_groups",
        )
    }
