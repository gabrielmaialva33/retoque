#!/usr/bin/env python3
"""Compile the real portable parser and repository ImGui hash with ASan/UBSan."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
# The menus whose text goes through Localization (the LSFG panel is the study branch's, retoque/lsfg/LsfgMenu.cpp).
MENU_SOURCES = ("retoque/dlssnr/DlssNr_Menu.cpp", "OptiScaler/menu/menu_common.cpp", "retoque/lsfg/LsfgMenu.cpp")
sys.path.insert(0, str(HERE))
from hack_font import hack_codepoints  # noqa: E402

OPTIONAL = ROOT / "retoque/translations/optional"
# pt-BR is the reference pack (reviewed); the others are machine-translated drafts held to the same key set.
PACKS = ("pt-BR", "de", "es", "fr", "ja", "ko", "pl", "ru", "zh-CN")
CJK_PACKS = ("ja", "ko", "zh-CN")
assert sorted(path.name for path in OPTIONAL.iterdir()) == sorted(f"{pack}.lang" for pack in PACKS), \
    "translations/optional must hold exactly the packs the packagers ship"
assert not (ROOT / "retoque/translations/OptiScaler.lang").exists(), "no active dictionary in the source tree"


def unescape(field):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t", "r": "\r", "\\": "\\"}[m.group(1)], field)


def entries(pack):
    result = {}
    for line in (OPTIONAL / f"{pack}.lang").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            source, translated = line.split("\t")
            result[unescape(source)] = unescape(translated)
    return result


def printf_directives(text):
    return re.findall(r"%[-+ #0']*(?:\*|\d+)?(?:\.(?:\*|\d*))?(?:I64|I32|ll|hh|[hljztLw])?[diuoxXfFeEgGaAcCsSp%]", text)


pack_text = (OPTIONAL / "pt-BR.lang").read_text(encoding="utf-8")
reference = entries("pt-BR")
for translated in reference.values():
    # With UseHQFont=false the menu draws ProggyClean, which has Latin-1 only; pt-BR keeps to it.
    assert all(ord(char) <= 255 for char in translated), "pt-BR needs glyphs outside default Latin range"
hack = hack_codepoints(ROOT)
problems = []
for pack in PACKS[1:]:
    text = (OPTIONAL / f"{pack}.lang").read_text(encoding="utf-8")
    header = "\n".join(line for line in text.splitlines()[:8] if line.startswith("#"))
    if "machine-translated draft" not in header or "native review" not in header:
        problems.append(f"{pack}: header must say it is a machine-translated draft pending native review")
    if "NeuRotic" not in header or "beae4eea" not in header or "GPL-3.0" not in header:
        problems.append(f"{pack}: header must credit NeuRotic's drafts (GPL-3.0, beae4eea)")
    translated = entries(pack)
    unknown = sorted(set(translated) - set(reference))
    missing = sorted(set(reference) - set(translated))
    problems += [f"{pack}: unknown key {key[:70]!r}" for key in unknown[:10]]
    problems += [f"{pack}: missing key {key[:70]!r}" for key in missing[:10]]
    for source, value in translated.items():
        if printf_directives(source) != printf_directives(value):
            problems.append(f"{pack}: printf directives differ for {source[:60]!r}")
        if "##" in value:
            problems.append(f"{pack}: '##' in the translation of {source[:60]!r}")
        if pack not in CJK_PACKS:
            absent = sorted({char for char in value if char != "\n" and ord(char) not in hack})
            if absent:
                problems.append(f"{pack}: {''.join(absent)!r} not in the menu's Hack font ({source[:40]!r})")
assert not problems, "language packs:\n" + "\n".join(problems[:60])
print(f"PASS: {len(PACKS)} packs share pt-BR's {len(reference)} keys, keep printf directives in order, "
      f"no '##'; Latin and Cyrillic packs draw only Hack glyphs ({len(hack)} codepoints)", flush=True)
with tempfile.TemporaryDirectory(prefix="nr-localization-") as temp:
    binary = Path(temp) / "cases"
    label_pattern = re.compile(r'Localization::Label\(((?:"(?:\\.|[^"\\])*"\s*)+)\)')
    labels = []
    for relative in MENU_SOURCES:
        labels.extend(label_pattern.findall((ROOT / relative).read_text()))
    assert len(labels) >= 60, "ZERO/LOW COVERAGE: expected actual production menu label call sites"

    # The redesigned menu translates in a few central places instead of at each call site: every
    # section and card title (SectionTitle, ScopedCard, SeparatorWithHelpMarker) and every card name in
    # the Custom tab picker go through Label; the sidebar's tab names, and the picker's tab headings,
    # through Tr. Since upstream 76219dd4 the sidebar groups are lowercase keys split by a line, never
    # shown, so only the tab names are text. Their literals are collected here so a title added upstream cannot stay in
    # English unnoticed.
    menu_source = (ROOT / "OptiScaler/menu/menu_common.cpp").read_text()
    literal = r'("(?:\\.|[^"\\])*")'
    central_labels = []
    for pattern in (r'\bSectionTitle\(' + literal, r'\bScopedCard card \{ ' + literal, r'\bScopedCard card\(' + literal,
                    r'\bSeparatorWithHelpMarker\(\s*' + literal):
        central_labels.extend(re.findall(pattern, menu_source))
    menu_boxes = re.findall(r'\{ "[a-z_0-9]+", ' + literal + r', ' + literal, menu_source)
    menu_tabs = re.findall(r'\{ "(?:main|system|advanced)", ' + literal, menu_source)
    assert len(central_labels) >= 30 and len(menu_boxes) >= 30 and len(menu_tabs) >= 8, \
        "ZERO/LOW COVERAGE: the menu's central title, card and tab tables were not found"
    labels.extend(central_labels + [name for _, name in menu_boxes])
    central_text = [tab for tab, _ in menu_boxes] + menu_tabs

    # Drift guard for display text: every literal the two menus send through Tr, or through the NR
    # menu's HelpMarker (which calls Tr), has an entry. Labels are checked in cases.cpp.
    def c_text(literals):
        escapes = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'"}
        return re.sub(r"\\(.)", lambda m: escapes[m.group(1)], "".join(re.findall(r'"((?:\\.|[^"\\])*)"', literals)))
    pack_keys = set()
    for line in pack_text.splitlines():
        if line and not line.startswith("#"):
            pack_keys.add(re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t", "r": "\r", "\\": "\\"}[m.group(1)],
                                 line.split("\t")[0]))
    text_pattern = re.compile(r'(?:Localization::Tr|\bHelpMarker)\(((?:"(?:\\.|[^"\\])*"\s*)+)\)')
    missing = []
    for relative in MENU_SOURCES:
        for literals in text_pattern.findall((ROOT / relative).read_text()):
            text = c_text(literals)
            if re.search(r"[A-Za-z]{2}", text) and text not in pack_keys:
                missing.append(f"{relative}: {text[:80]!r}")
    for literals in central_text:
        if c_text(literals) not in pack_keys:
            missing.append(f"OptiScaler/menu/menu_common.cpp (tab or group name): {c_text(literals)[:80]!r}")
    assert not missing, "pt-BR pack lacks display text:\n" + "\n".join(missing)
    (Path(temp) / "menu-labels.h").write_text(
        "static const char* menuLabels[] = {\n" + ",\n".join(labels) + "\n};\n")
    subprocess.run([
        "g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-I", temp, "-I", str(ROOT / "retoque"), "-I", str(ROOT / "OptiScaler"), "-I", str(ROOT / "OptiScaler/include/imgui"),
        str(HERE / "cases.cpp"), str(ROOT / "OptiScaler/include/imgui/imgui.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)] + [str(OPTIONAL / f"{pack}.lang") for pack in PACKS], check=True)

    # The menu font decision over every pack, against the codepoints the embedded Hack font has.
    fonts = Path(temp) / "fonts"
    (Path(temp) / "hack-codepoints.txt").write_text("\n".join(str(c) for c in sorted(hack)))
    subprocess.run([
        "g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / "retoque"), str(HERE / "fonts.cpp"), "-o", str(fonts),
    ], check=True)
    subprocess.run([str(fonts), str(Path(temp) / "hack-codepoints.txt")] +
                   [f"{pack}={OPTIONAL / f'{pack}.lang'}" for pack in PACKS], check=True)

with tempfile.TemporaryDirectory(prefix="nr-localization-runtime-") as temp:
    directory = Path(temp)
    binary = directory / "runtime"
    subprocess.run([
        "g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(HERE / "stubs"), "-I", str(ROOT / "retoque"), "-I", str(ROOT / "OptiScaler"),
        str(HERE / "runtime.cpp"), str(ROOT / "retoque/misc/Localization.cpp"), "-o", str(binary),
    ], check=True)
    for scenario in ("absent", "inactive", "invalid", "active", "active-ja"):
        case = directory / scenario
        case.mkdir()
        if scenario == "inactive":
            optional = case / "translations/optional"
            optional.mkdir(parents=True)
            for pack in PACKS:
                (optional / f"{pack}.lang").write_bytes((OPTIONAL / f"{pack}.lang").read_bytes())
        if scenario == "invalid":
            (case / "OptiScaler.lang").write_text("Save Settings\tSalvar configurações\nunsafe\t%n\n")
        if scenario == "active":
            (case / "OptiScaler.lang").write_bytes((OPTIONAL / "pt-BR.lang").read_bytes())
        if scenario == "active-ja":
            (case / "OptiScaler.lang").write_bytes((OPTIONAL / "ja.lang").read_bytes())
        subprocess.run([str(binary), scenario], cwd=case, check=True)
