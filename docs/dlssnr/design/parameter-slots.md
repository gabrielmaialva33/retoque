# The capability block's vtable slots, by type

Status: 2026-10-31, read off one model binary (`nvngx_dlssnr.dll` `6eb209e7…`, the kit's) and one loopback run. No
code changed behaviour on the strength of this note; it corrects the loaders' comments and records why the six
"mismatches" below are left alone.

## Why

The model's VRAM figure never appeared because the loaders cleared and read `SizeInBytes` through the wrong pair of
slots ([model-vram-stats](model-vram-stats.md), "The slot the size is written through"). The loaders named their
slots after the header's declaration order, and that order is not the vtable's. The same naming covers every
parameter the fork sends, so every one was checked.

## The layout

`NVSDK_NGX_Parameter` declares its eight `Set` overloads, then its eight `Get` overloads, as ULL, float, double, uint,
int, `ID3D11Resource*`, `ID3D12Resource*`, `void*`. MSVC lays each overload group out in reverse:

| Value type | Setter slot | Getter slot | Seen in the model |
|---|---:|---:|---|
| `void*` | 0 | 8 | all seven resources read through 8 |
| `ID3D12Resource*` | 1 | 9 | predicted only |
| `ID3D11Resource*` | 2 | 10 | predicted only |
| `int` | 3 | 11 | 34 integers (flags, preset, subrects) read through 11 |
| `unsigned int` | 4 | 12 | six integers read through 12 |
| `double` | 5 | 13 | predicted only |
| `float` | 6 | 14 | `ScalingRatio` written through 6; seven floats read through 14 |
| `unsigned long long` | 7 | 15 | `SizeInBytes` written through 7 |

This is also why the float setter has always been discovered at slot 6 rather than the header's 1: slot 1 is the
`ID3D12Resource*` setter. In the loaders, `VT_SET_ULL` (0) is the `void*` setter and `VT_SET_UINT` (3) is `Set(int)`;
the names are kept because tests hold the two loaders to each other by them.

## The audit

Every literal name the forwarder and the direct runtime write (55, the same in both) was found in the model by its
string's address and the vtable offset of the call that follows. Two analyses were made independently, one by each
agent, and agreed name for name:

- 48 match: the resources (0 → 8), the floats (6 → 14) and 34 integers (3 → 11).
- 6 are written as `int` and read as `unsigned int` (3 → 12): `DLSSNR.Width`, `DLSSNR.Height`, `CreationNodeMask`,
  `VisibilityNodeMask`, `PerfQualityValue`, `DLSSNR.Style`.
- `SizeInBytes` is only written by the model (four sites, all slot 7); the fork reads it through 15 since `b74d6a65`.

The six arrive. In a `tests/dlssnr-loopback` run with `Style=1` and `Preset=2`, the host's own readback, which goes
through `Get(unsigned int*)` (slot 12, as the model reads `Style`), returned `DLSSNR.Style -> 1` and
`DLSSNR.Hint.Render.Preset -> 2`, both with result `0x1`. The block converts between `int` and `unsigned int`; it does
not convert between a pointer and a number (`SizeInBytes`), nor a float into a resource slot (the float discovery).
They are therefore left on slot 3: moving them would change what the model is sent for no measured gain.

## Names the model reads that the fork never writes

- `DLSSNR.ControlMask` and its subrect: known, see [hud-protection](hud-protection.md) and
  [model-310-9-16](model-310-9-16.md).
- `DLSSNR.BidirectionalDistortionField` and its subrect (`BaseX`, `BaseY`, `Width`, `Height`): not recorded anywhere
  in the fork before this note. See below.

### `DLSSNR.BidirectionalDistortionField`

Read statically from the same binary (2026-10-31):

- **Optional.** One read site per name, in the model's common parameter reader: the field through the `void*` getter
  (8), its four subrect values through the `int` getter (11). A failed or null read leaves the field null and the
  subrect zero, and evaluation goes on; only `Color` and `Output` are required ("Missing Color or Output parameter").
- **No checks found.** No format, size or rectangle validation of this field, unlike `Color`, `Output` and
  `Backbuffer`. When it is present the evaluator sets one bit (`0x4`) in the evaluation-info flags it hands on; no
  network-side use of the texture was found, so it is not shown that supplying it changes the image.
- **Its contract is Streamline's.** `sl::kBufferTypeBidirectionalDistortionField` (50, `external/streamline/sl_core_types.h`)
  is optional, four channels in normalized pixel space: RG distorted → undistorted, BA undistorted → distorted. The
  vendored NGX SDK has no such parameter.
- **What the fork receives.** The Streamline hooks recognize tag 50, but `Streamline_Inputs.cpp`'s resource mapper
  drops it, so it is never kept. The FFX frame-generation input does keep a distortion field and FSR FG uses it, but
  that one is two channels and one direction, a different contract.

Sending it to NR would need the tag kept from a game that tags it, then a GPU A/B in a title with lens distortion to
show the model uses it at all. Not started.

## Limits

- One model binary. Another version may read a name through another overload; the conversion between `int` and
  `unsigned int` is the driver block's and was seen with one driver core (the loopback kit's `_nvngx.dll`, under Proton).
- Names built at run time or hidden from a string search would not be found; none is known.
- A match is a matching slot pair, not proof that the value has the effect intended.
