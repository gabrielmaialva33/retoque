# Hardware architecture for Retoque decisions

`NvidiaArchitecture.h` is a pure, bounded correction for a newer NVAPI architecture reported on an
older adapter (for example, dlssg_for_sm86 reporting Ada on a GA106 RTX 3060). It recognizes PCI device
`0x2504` and canonical GeForce RTX 20/30 adapter names. Unknown IDs/names, non-NVIDIA vendors and zero
NVAPI architecture retain the reported value. It only demotes; it never upgrades an adapter.

`NvidiaArchitectureGpu.h` bridges `GpuInformation` to that policy and logs the first mismatch once
per process. It does not mutate the GPU cache or hook NVAPI. NR/RR automatic working scale, the MFG
probe and the automatic Ada kernel gate use it; explicit user settings keep their existing semantics.

The correction does not depend on loader visibility: IdentifyGpu and the MFG gate can cache their
answers before the foreign loader's backend module becomes visible. A loader alone cannot identify
an adapter. Professional names and unknown PCI devices without a recognized name remain uncorrected.

`tests/mfg-probe` covers the pure policy and its scale/probe decisions on the host. Windows compilation
and real RTX 20/30 sessions with the loader are separate qualification steps.
