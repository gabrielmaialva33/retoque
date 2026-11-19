#!/usr/bin/env python3
"""Verify locale-independent float serialization, single-comma recovery and NR log formatting."""
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(source: str, signature: str, label: str) -> str:
    start = source.find(signature)
    if start < 0:
        fail(f"{signature!r} not found in {label}")
    open_brace = source.index("{", start)
    depth = 0
    i = open_brace
    while i < len(source):
        if source.startswith("//", i):
            newline = source.find("\n", i)
            i = len(source) if newline < 0 else newline + 1
            continue
        c = source[i]
        if c in ("\"", "'"):
            i += 1
            while i < len(source) and source[i] != c:
                i += 2 if source[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
        i += 1
    fail(f"unbalanced braces after {signature!r} in {label}")


config_h = (ROOT / "OptiScaler/Config.h").read_text(encoding="utf-8-sig")
config_cpp = (ROOT / "OptiScaler/Config.cpp").read_text(encoding="utf-8")
scan_cpp = (ROOT / "retoque/dlssnr/DlssNr_ExposureScan.cpp").read_text(encoding="utf-8")
model_cpp = (ROOT / "retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp").read_text(encoding="utf-8")
present_cpu_h = (ROOT / "retoque/dlssnr/DlssNr_PresentCpuTiming.h").read_text(encoding="utf-8")

optional_slice = config_h[config_h.index("enum HasDefaultValue") : config_h.index("constexpr inline int UnboundKey")]
is_float = body(config_cpp, "static inline bool isFloat(", "OptiScaler/Config.cpp")
get_float = body(config_cpp, "std::string GetFloatValue(", "OptiScaler/Config.cpp")
overlay_types = config_cpp[
    config_cpp.index("enum class OverlayValueType") : config_cpp.index("void Config::ReadOverlaySettings(")
]
overlay_key_prefix = 'static constexpr std::string_view overlayKeyPrefix = "FpsOverlay";'
overlay_code_key = body(config_cpp, "static std::string_view OverlayCodeKey(", "OptiScaler/Config.cpp")
pack_overlay_fn = body(config_cpp, "static void PackOverlayValue(", "OptiScaler/Config.cpp")
unpack_overlay_fn = body(config_cpp, "static std::optional<std::string> UnpackOverlayValues(", "OptiScaler/Config.cpp")
overlay_helpers = f"{overlay_types}\n{overlay_key_prefix}\n{overlay_code_key}\n{pack_overlay_fn}\n{unpack_overlay_fn}"
read_string = body(config_cpp, "std::optional<std::string> Config::readString(", "OptiScaler/Config.cpp")
read_float = body(config_cpp, "std::optional<float> Config::readFloat(", "OptiScaler/Config.cpp")
load_anchors = body(scan_cpp, "void LoadAnchors(const std::string& serialized)", "DlssNr_ExposureScan.cpp")
serialize_anchors = body(scan_cpp, "std::string SerializeAnchors()", "DlssNr_ExposureScan.cpp")
report_ratios = body(model_cpp, "void ReportScalingRatios()\n{", "DlssNr_Dx12_Model.cpp")

harness = (pathlib.Path(__file__).resolve().parent / "cases.cpp").read_text(encoding="utf-8")
replacements = {
    "// @@CUSTOM_OPTIONAL@@": optional_slice,
    "// @@CONFIG_HELPERS@@": f"{is_float}\n\n{get_float}\n\n{overlay_helpers}",
    "// @@CONFIG_METHODS@@": f"{read_string}\n\n{read_float}",
    "// @@ANCHOR_METHODS@@": f"{load_anchors}\n\n{serialize_anchors}",
    "// @@REPORT_SCALING_RATIOS@@": report_ratios,
}
for marker, snippet in replacements.items():
    if marker not in harness:
        fail(f"missing marker {marker} in cases.cpp")
    harness = harness.replace(marker, snippet)

with tempfile.TemporaryDirectory(prefix="nr-config-locale-") as temp_dir:
    temp = pathlib.Path(temp_dir)
    generated = temp / "generated.cpp"
    binary = temp / "test"
    generated.write_text(harness, encoding="utf-8")
    subprocess.run(
        [
            "g++",
            "-std=c++20",
            "-O1",
            "-g",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-isystem",
            str(ROOT / "external/simpleini"),
            "-I" + str(ROOT / "retoque/dlssnr"),
            str(generated),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

# Source guards against locale-sensitive C/C++ formatting and parsing re-entering these paths.
clean_get_float = strip_comments(get_float)
for forbidden in ("to_string", "snprintf", "sprintf"):
    if forbidden in clean_get_float:
        fail(f"GetFloatValue uses locale-sensitive {forbidden}")

clean_is_float = strip_comments(is_float)
for forbidden in ("istringstream", "stof", "strtof", "sscanf"):
    if forbidden in clean_is_float:
        fail(f"isFloat uses locale-sensitive {forbidden}")

pack_overlay = strip_comments(body(config_cpp, "static void PackOverlayValue(", "OptiScaler/Config.cpp"))
if "strtof" in pack_overlay or "stof" in pack_overlay:
    fail("PackOverlayValue uses locale-sensitive strtof/stof")

for label, snippet in (
    ("DlssNr_PresentCpuTiming.h::Format", body(present_cpu_h, "inline std::string Format(", "DlssNr_PresentCpuTiming.h")),
    ("DlssNr_Dx12_Model.cpp::ReportScalingRatios", report_ratios),
    ("DlssNr_ExposureScan.cpp::LoadAnchors", load_anchors),
    ("DlssNr_ExposureScan.cpp::SerializeAnchors", serialize_anchors),
):
    cleaned = strip_comments(snippet)
    for forbidden in ("snprintf", "sprintf", "stof", "strtof", "to_string"):
        if forbidden in cleaned:
            fail(f"{label} uses locale-sensitive {forbidden}")

print("PASS: source guards confirm classic-locale to_chars/from_chars across Config, ExposureScan and NR logs")
