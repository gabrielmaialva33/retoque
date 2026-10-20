"""The NR renderer's D3D12 sources as one text, for suites that assert on it or slice it.

retoque/shaders/dlssnr/DlssNr_Dx12.cpp is being split into responsibility units. A suite that reads "the renderer"
reads all of them: the main unit first, then every DlssNr_Dx12_*.cpp in name order, each opened by a marker line
naming its file, so a slice that crosses a file boundary shows where. Plain-string assertions keep working wherever
the code moves; slices that compile a region still need that region to live in one file.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "retoque/shaders/dlssnr"


def renderer_paths():
    return [RENDERER / "DlssNr_Dx12.cpp"] + sorted(RENDERER.glob("DlssNr_Dx12_*.cpp"))


def renderer_source():
    # utf-8-sig: a byte-order mark is not code, and one suite already read the file that way.
    return "".join(f"// ==== {p.name} ====\n" + p.read_text(encoding="utf-8-sig") for p in renderer_paths())
