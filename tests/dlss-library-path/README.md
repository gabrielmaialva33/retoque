# DLSS library path

`python3 tests/dlss-library-path/run.py` slices dllmain's `[Libraries] NvngxDlssPath` block from
`OptiScaler/dllmain.cpp` and runs it on real folders under ASan/UBSan.

`State::NVNGX_DLSS_Path` holds the path of `nvngx_dlss.dll` itself when a search finds it, and its users take the
folder from it: `NVNGXProxy::GetFeatureCommonInfo` with `remove_filename`, the NVNGX inputs' override entry with
`parent_path`. The override kept the folder instead, so NGX was handed the folder's parent unless the setting ended
in a separator, and a setting that named the file was ignored. kkyleeb21's D18 0.5.1 (`3de7b799`) fixed the same in
their tree.

Cases: a folder with and without a trailing separator, the file itself (all three keep the file, and both users get
the folder), a folder without the file and a path that does not exist (nothing kept, the search runs). Before the
fix six checks failed. The fake `State` accepts a `std::filesystem::path` the way MSVC's implicit conversion does,
so the block compiles unchanged on both sides of the fix.
