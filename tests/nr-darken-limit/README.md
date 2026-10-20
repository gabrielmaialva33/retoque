# Final darkening floor

`python3 tests/nr-darken-limit/run.py` compiles the actual HLSL helper and final writeback as float32 C++, with ASan/UBSan. Literal luminance oracles cover 75% (single application), black, signed HDR, near-zero signals, passthrough domain, stricter existing results and bit-identical disabled writeback including nonfinite inputs. It also checks sanitization and all API wiring.

This does not execute DXIL/SPIR-V, NGX, temporal stabilization or game postprocessing. It cannot establish GPU bit identity or image quality. The RED run on the unchanged shader failed the literal 75% oracle.
