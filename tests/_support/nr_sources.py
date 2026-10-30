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


def present_identity_glue():
    """Assemble uniquely owned production declarations and glue for host fixtures."""
    import re
    core = renderer_with_internal_types()
    header = renderer_internal_header()
    guide = (RENDERER / "DlssNr_Dx12_PresentGuides.cpp").read_text()

    def unique(text, pattern, label):
        matches = list(re.finditer(pattern, text, re.M))
        assert len(matches) == 1, f"expected one {label}, found {len(matches)}"
        return matches[0]

    state_start = unique(core, r"^struct PresentGuideIdentityState$", "identity state").start()
    identity = unique(core, r"^PresentGuideIdentityState Dx12Internal::g_guideIdentity;", "identity storage")
    status = unique(core, r"^std::atomic<uint64_t> Dx12Internal::g_guideStatus\b[^;]*;", "status storage")
    status_at = unique(core, r"^std::atomic<long long> Dx12Internal::g_guideStatusAtMs\b[^;]*;", "status clock")
    fallback = unique(core, r"^std::atomic<uint8_t> Dx12Internal::g_guideFallback\b[^;]*;", "fallback state")
    assert state_start < identity.start() < status.start() < status_at.start() < fallback.start()
    state = core[state_start:fallback.end()]
    ask = unique(header, r"^struct GuideAsk\n\{[\s\S]*?^\};", "GuideAsk").group()
    bridge = unique(core, r"^std::atomic<uint64_t> Dx12Internal::g_bridgePresentedExtent\b[^;]*;", "bridge extent").group() + "\n"
    start = unique(guide, r"^static DlssNr::PresentGuideIdentity::Stamp EvaluateOutput\(", "glue start").start()
    end = unique(guide, r"^struct GuideCapture$", "glue end").start()
    assert start < end
    prefix = "namespace Dx12Internal\n{\n"
    assert guide[start - len(prefix):start] == prefix
    start -= len(prefix)
    assert guide[end - len(prefix):end] == prefix
    end -= len(prefix)
    body = guide[start:end]
    marker = "// GuideAsk is declared in DlssNr_Dx12_Internal.h."
    assert body.count(marker) == 1, "expected one GuideAsk marker"
    body = body.replace(marker, ask)
    body = body.replace("namespace Dx12Internal\n{\n", "").replace("} // namespace Dx12Internal\n", "")
    return (state + bridge + body).replace("Dx12Internal::", "")
