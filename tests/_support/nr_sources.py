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


def renderer_internal_header():
    return (RENDERER / "DlssNr_Dx12_Internal.h").read_text(encoding="utf-8-sig")


def renderer_with_internal_types():
    """Source-slice fixtures: put the actual shared declarations at their old anchors.

    This is NOT a compilation model for the DLL. It only lets existing fake-host region
    tests consume the moved declarations without maintaining copies. Linkage is tested
    separately by nr-dx12-internal; no namespace/storage qualifiers are changed here.
    """
    import re
    header = renderer_internal_header()

    def declaration(match):
        name = match.group(1)
        start = header.index("struct " + name + "\n{")
        # Top-level types close at column zero; nested types are indented.
        stop = header.index("\n};", start) + len("\n};")
        return header[start:stop]

    return re.sub(r"// Internal type: (\w+) \(DlssNr_Dx12_Internal.h\)",
                  declaration, renderer_source())
