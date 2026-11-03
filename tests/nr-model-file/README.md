# NR model file

`python3 tests/nr-model-file/run.py`

`[DlssNr] ModelFile` names the NR model file Retoque's loaders use ([model-file.md](../../docs/dlssnr/design/model-file.md)).

The suite compiles `retoque/dlssnr/DlssNr_ModelFile.h` under ASan/UBSan on a fake filesystem. The cases cover:

- auto and its spellings;
- a named file beside OptiScaler first, then beside the game;
- a missing file that is refused and never replaced by the default;
- full paths (drive and UNC), with labels that never write out a folder of the user's;
- partial roots (`C:x`, `\x`), which are refused;
- UTF-8 names;
- whitespace and backslashes;
- a directory named like a model;
- no working-directory search;
- containment.

Twelve rule mutants must fail those cases.

Source guards read the production tree and check:

- only the resolver and the driver route's `NgxInfo::FindModel` search by the literal name;
- only the resolver looks the loaded module up by base name;
- auto is the old search, from each site's own folder, with the old reason text;
- the two D3D12 create sites, the scaling ratios, the D3D11 probe, Vulkan and the pair capture ask the resolver;
- UseProxy keeps the driver's file and refuses a named one before its gate and its create;
- identity and log labels follow the named file, and a named model is looked up by its full path;
- paths are made from UTF-8, not through the code page;
- the four config points, frozen by the constructor and kept out of the log's parameter list;
- the design record.

Eleven source mutants must each fail a guard.

Not proved here: that a renamed model loads and runs through the forwarder, the direct runtime or Vulkan, that
the model's caller checks accept it, MSVC compilation (the Release build covers that), or native Windows.
