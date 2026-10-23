# Guide copy contract

`python3 tests/nr-guide-copy-contract/run.py` compiles the production ReadableGuide with host D3D12 fakes and ASan/UBSan.
The copy checks reject incompatible width, height, dimension, array/depth, mip and sample count/quality. Each change retires the old clone before copying; unchanged geometry reuses a successful fallback format despite different source flags/alignment. Allocation failure and null/typed passthrough are covered.
No device, NGX, GPU copy, resource-state validation or Windows driver runs here.
