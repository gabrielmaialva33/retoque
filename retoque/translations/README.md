# Optional menu text packs

The package ships nine dictionaries under `optional/`, all **inactive**:

| File | Language | Status |
|---|---|---|
| `pt-BR.lang` | Portuguese (Brazil) | reviewed; the reference pack the others follow |
| `de.lang`, `es.lang`, `fr.lang`, `pl.lang`, `ru.lang` | German, Spanish, French, Polish, Russian | machine-translated drafts, pending native review |
| `ja.lang`, `ko.lang`, `zh-CN.lang` | Japanese, Korean, Simplified Chinese | machine-translated drafts, pending native review; need a system CJK font (below) |

The drafts cover exactly the entries of `pt-BR.lang`. Where NeuRotic's machine-translated drafts
(`localization/machine-drafts/<locale>.nrlang` in that GPL-3.0 fork, commit `beae4eea`) had the identical
English text in their in-game set, their translation was taken (each file's header says how many); the rest
were translated for these packs. Corrections from native speakers are welcome: keep the key column exactly as
it is and follow the format rules below.

The menu only loads a file named `OptiScaler.lang` directly beside the OptiScaler DLL, once per process. It
does not search this directory, inspect the system locale, or offer a language switch. Without that file,
every lookup returns the original English string. Unknown entries also stay in English.

To prepare activation, copy one `optional/<locale>.lang` to `OptiScaler.lang` beside the DLL in an
**isolated staging directory**. This instruction does not install anything into a game.
Only deploy that staged selection as part of an explicitly authorized game test. Restart the
application after changing or removing the active dictionary. Remove/rename `OptiScaler.lang`
and restart to return to English. The optional directory can safely remain in place.

## Fonts

The menu draws with the embedded Hack font (or ImGui's ProggyClean with `UseHQFont=false`, or the file
`TTFFontPath` names). Hack has every character the Latin and Cyrillic packs use; the host test checks that
against the embedded font. ProggyClean has Latin-1 only: with `UseHQFont=false`, Polish and Russian letters
outside it show as `?` (pt-BR keeps to Latin-1, and its test enforces that).

Japanese, Korean and Chinese need glyphs neither font has. When the active `OptiScaler.lang` contains them,
`retoque/misc/MenuFonts` merges one system face into the menu font when the menu starts, behind Hack, so
Hack keeps every character it has. The face's glyph ranges are the codepoints the pack uses; this ImGui (1.92,
whose menu backends all rasterize glyphs on demand) only uses ranges to preload, so in practice the face also
supplies any other character Hack lacks, and only characters actually drawn take atlas space:

| Pack script | Faces tried in `%WINDIR%\Fonts`, in order |
|---|---|
| Hangul (ko) | `malgun.ttf`, `gulim.ttc` |
| Kana (ja) | `YuGothM.ttc`, `meiryo.ttc`, `msgothic.ttc`, `YuGothR.ttc` |
| Ideographs only (zh-CN) | `msyh.ttc`, `msyh.ttf`, `simsun.ttc`, `Deng.ttf`, `simhei.ttf` |

Proton links `malgun.ttf`, `msgothic.ttc`, `msyh.ttf` and `simsun.ttc` into each prefix's `C:\windows\Fonts`, so
the three packs find a face there too. If none of the faces exists, the log says so once and the atlas stays as
it is; the missing characters then show as `?`. Nothing is shipped: those faces are large and not ours to
redistribute. The chosen face is read into memory whole (up to about 20 MB for Proton's CJK files).

Limits: the decision is made once per process from the dictionary's text; the FPS overlay's separate font
gets no CJK face; a Korean pack quoting Hanja uses the Korean face; and English, pt-BR and the Latin and
Cyrillic packs never reach the font code, so their atlas is byte for byte what upstream builds. The font merge
has been checked by host tests and a Release build only; how it looks in a running game has not been checked.

## What it covers

Each pack covers the DLSS Neural Rendering panel, synthesized frame generation's settings, the
section headers and menu shell already routed through `Localization`, and the status and failure
reasons the NR panel shows. On the LSFG study branch it also covers the LSFG block under FSR FG
Settings (`retoque/lsfg/LsfgMenu.cpp`); LSFG's own fallback reasons in its status line stay English. Since upstream's menu redesign (merged 2026-10-04) the shell is translated
in a few central places rather than at each call site: `SectionTitle` and `ScopedCard` (every section
and card title), the sidebar's tab and group names, and the Custom tab picker. The sidebar widens to
fit the longest translated name. The Vulkan present-mode and HUDless-flip controls added on 2026-11-09,
and the FSR debug-view control, also use the optional pack. The rest of upstream's menu stays in English: routing
its strings would change most of `menu_common.cpp`, an upstream file, and every merge from
`optiscaler/master` would conflict on it. Dynamic diagnostic text absent from the dictionary also
falls back to English. This is not a runtime language switcher.

English is the default and is unchanged without the file: `Tr` returns the original pointer and
`Label` the original string, byte for byte. Filter names (FSR1, Lanczos3, ...) are kept in English
and listed as themselves, because every production label must have an entry (see the tests).

## Labels and ImGui identity

ImGui identifies a control by hashing its label, and `ImHashStr` restarts at `###`, so everything
after it is the control's whole identity. `Label` keeps that identity stable under translation:

- a label that already has `###` keeps it: `Reset###fixed` becomes `Redefinir###fixed`;
- a bare or `##` label gets its whole original text after a new `###`: `Reset##detail` becomes
  `Redefinir###Reset##detail`, and `Enable Neural Rendering` becomes
  `Ativar renderização neural###Enable Neural Rendering`.

The second case changes the control's ID from `hash(original)` to `hash("###" + original)`. That is
safe here: the pack loads once per process, so the ID is fixed for the whole run, and nothing stores
ImGui IDs across runs (`menu_common.cpp` sets `io.IniFilename` to null). Two labels share an ID after
translation exactly when their English originals did, so no two controls merge; the suite checks that
over every production label. Until 2026-09-28 bare and `##` labels stayed in English to keep the
original hash, which left most of the panel's controls untranslated.

## Dictionary format

UTF-8 escaped TSV, one exact English source and one translation separated by one literal tab.
Blank lines and lines starting with `#` are ignored. An initial UTF-8 BOM and CRLF are accepted.
Use `\n`, `\t`, `\r` and `\\` inside fields. Duplicate keys, bad UTF-8, NUL/control bytes,
unknown escapes, extra tabs, empty translations, injected `##`, incompatible printf formats,
`%n`, positional formats and oversized files/lines reject the entire dictionary.

Keep format directives exactly in order, including width, precision, `*`, length modifiers
and `%%` literals. This intentionally rejects harmless-looking format changes as well as
unsafe ones. Unsupported printf dialects or bare `%` characters cannot be translated in this
first parser; those English strings may simply be omitted from the pack.

The optional-dictionary idea was inspired by Moeblack's localization work in
[PR #26](https://github.com/Dagherbou/OptiScaler_DLSSNR/pull/26). The loader, validation and ID
policy here are a separate implementation adapted to this repository's invariants.
