# NR crash note

```bash
python3 tests/nr-crash-note/run.py
```

The last-chance crash note ([engagement-funnel.md](../../docs/dlssnr/design/engagement-funnel.md), "The crash note").
The runner compiles `retoque/dlssnr/DlssNr_CrashNote.h` as it is (standard library and the funnel only) with g++
C++20, ASan/UBSan and `-Werror`, with every `operator new` in the program counted, runs `cases.cpp`, and checks
`DlssNr_CrashNote.cpp` and its call sites by source.

## What this covers

- **The note.** Three lines in the log's own `[hh:mm:ss.ffffff] [E]` shape: the code and its name (access violations
  with read/write/execute and the target), the module by export name plus offset, the executable, an unnamed image
  or no image; the last NR stage on this thread or another; the funnel line with the top decline. No path.
- **Bounds.** Any buffer size, 0 included, is filled, terminated and never overrun; the full note fits `kNoteBytes`.
  The stack-overflow line is fixed text.
- **The image name.** A synthetic PE32+ image: the export name, cut to its buffer; refused for no MZ, no PE
  signature, PE32, a directory or name past the image or past what is mapped, an unterminated or empty name; a name
  holding separators is sanitised.
- **No allocation.** Formatting, naming the image and reading the funnel allocate nothing.
- **Source guards.** One `SetUnhandledExceptionFilter` install and the conditional restore, nothing else; the filter
  continues the search when re-entered and otherwise ends by calling the filter it replaced (or continuing the search),
  never handling the crash; the note's writer contains its own faults, calls no logger, `Config`, allocator or
  loader-lock function, and writes a stack overflow's fixed line; `InstallOnce` is one relaxed load after the first
  call; the install appends to the existing log with full sharing and only with logging to a file on; the restore
  gives the slot back only while it is ours; no vectored handler anywhere in `OptiScaler/` or `retoque/`. The D3D12
  after-upscale and present entries install it after their NR-off return; `dllmain.cpp` uninstalls on an unload only;
  the project files compile the unit.

## What this does not cover

No Win32 code runs here: `VirtualQuery`, `WriteFile`, the filter chain and the unload are checked by source. Whether
the filter is reached under Proton and on native Windows, after an engine's own filter, is the in-game check in the
design note.
