# Optional localization verification

Run from the repository root:

```sh
python3 tests/nr-localization/run.py
```

Requires g++ with C++20 and ASan/UBSan. Temporary outputs are outside the repository; tests
are not part of the shipped DLL or release package. No game directory, marker, locale or
font configuration is changed.

The suite compiles the real portable dictionary parser and the repository's complete
`imgui.cpp`, linking its real `ImHashStr`. It extracts actual literal label call sites from
the menu files that route text through `Localization` (`MENU_SOURCES` in `run.py`: the NR panel,
`menu_common.cpp` and, on the LSFG study branch, `retoque/lsfg/LsfgMenu.cpp`) and requires at least
60 to prevent silent zero coverage. It checks
each label's ImGui identity across four seeds (an existing `###` ID kept; a translated bare or
`##` label identified by `###` plus its original text; untranslated text unchanged), that two
labels share an ID after translation exactly when they did in English, that every production
label has a pack entry, UTF-8, malformed files, transactional rejection, printf argument order,
width/precision stars, lengths, percent literals and the shipped Portuguese pack.

Before compiling, `run.py` also checks that every literal those menus send through `Tr`, or
through the NR menu's `HelpMarker`, has an entry, so text added to the menu cannot silently stay
in English. The redesigned menu translates its shell centrally, so the literals passed to
`SectionTitle`, `ScopedCard` and `SeparatorWithHelpMarker` and the names in the Custom tab picker
are added to the label set, and the sidebar's tab and group names are checked like `Tr` text. A string whose `%` is not a printf directive cannot be in the pack at all (the parser
rejects the whole file); reword it (`50%.` became `half size` in one tooltip).

Every pack in `retoque/translations/optional/` is checked, and the directory must hold exactly the nine the
packagers ship (pt-BR, de, es, fr, ja, ko, pl, ru, zh-CN). `run.py` requires each draft to carry the
machine-translation and NeuRotic credit header, to have exactly pt-BR's keys (no unknown key, none missing),
the same printf directives in the same order and no `##`; `cases.cpp` loads each with the real parser and
checks every production label's identity and collisions under it. pt-BR keeps to Latin-1 (ProggyClean,
`UseHQFont=false`). For the Latin and Cyrillic packs every character must be in the menu's embedded Hack font:
`hack_font.py` decodes `OptiScaler/menu/font/Hack_Compressed.h` (base85 + stb_compress, as ImGui does) and reads
its cmap.

`fonts.cpp` compiles `retoque/misc/MenuFonts.h` alone and checks the menu font decision: no pack and the
Latin/Cyrillic packs need nothing (no ranges, no font lookup, so the atlas is untouched); ja, ko and zh-CN ask
for a Japanese, Korean and Chinese face; every character of those packs is either in Hack or in the merged
ranges; ranges are sorted and merged; no existing candidate means no font; Proton's prefix fonts are found.
It does not load a CJK font or render anything.

A separate executable compiles production `Localization.cpp` with only DLL-path/logging/PCH
platform stubs. Five separate processes check absent, inactive-subdirectory (all packs), invalid,
explicitly active pt-BR and active Japanese dictionaries, including the font decision over what was loaded. They also check that edits after loading cannot invalidate
returned pointers or silently change language. Files exist only in temporary test directories.

Passing means every assertion succeeds and both executables exit 0 with sanitizer errors
absent. It does not prove Windows DLL integration, menu rendering, text layout or font glyphs;
Release compile/link and a later authorized UI check are separate validations.

See [pack documentation](../../retoque/translations/README.md) for activation, coverage and
the label identity rule.

## Adversarial review

- No config settings or NR values changed; no visibility condition or slider range changed.
  Two English help texts were reworded on 2026-09-28 (a bare `%`, and "Minimum base fps" now says
  it is not a target).
  The white-point source/anchor table in DEVELOPMENT.md remains structurally identical.
- Text uses `Tr`; widgets use `Label`. Hidden IDs never leak into help/status text.
- Dictionaries cannot inject format arguments or hidden IDs. Unknown or rejected text falls
  back to the exact original string; inactive packaging does not select a language.
- Bare and `##` labels are translated as `translation###original`. That changes their ID to
  `hash("###" + original)`, fixed for the process; nothing persists ImGui IDs (`IniFilename` is
  null), and the collision check proves no two controls merge. Existing `###` suffixes survive byte
  for byte, and without a pack every label is returned unchanged.
- No shader, constant-buffer, Vulkan resource or render-path changes. The only font change merges a system CJK
  face when the active pack needs one (`retoque/misc/MenuFonts`); without such a pack the atlas code is not reached.
- Runtime translation strings are immutable after first load; cached labels have stable
  storage and a mutex, with a defensive entry cap and English fallback.
