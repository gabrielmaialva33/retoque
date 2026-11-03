# Streamline tag inventory

`python3 tests/nr-sl-tag-inventory/run.py`

With NR on, Retoque records which buffers the game tags for Streamline and whether the HUD-less colour and the UI
colour with alpha would still be valid at the game's Present, and says it in one log line when that changes
([sl-tag-inventory.md](../../docs/dlssnr/design/sl-tag-inventory.md)). Log only: nothing reads it to decide.

The suite compiles `retoque/dlssnr/DlssNr_SlTagInventory.h` under ASan/UBSan against synthetic tags. The cases cover:

- nothing is noted while unarmed;
- the first line waits for a real tag, and removals are counted but are not buffers;
- the line's contents and the verdicts for the two buffers;
- one line while nothing changes, and a change said only after the two-second gap;
- mixed lifecycles;
- the D3D12 description filling the shape;
- subrects and the extent fallback;
- where each tag was seen;
- unnamed types, types past 95 and an undefined lifecycle;
- a buffer said as a buffer, never with its byte width as a size;
- the packing of the shape, including a Vulkan extension format number;
- notes from several threads while the line is taken (that the line and its number agree under a racing note
  is a source guard, not a case).

Sixteen rule mutants must fail those cases; only the one that changes a constant may be caught by the compiler.

Source guards check:

- the Streamline numbers against the vendored header;
- the line and its remembered number taken from one read of the inventory;
- the hook helper: nothing before arming, `GetDesc` only on D3D12 textures and buffers, buffers said as buffers;
- upstream's tag hooks noting first;
- the note-only hooks doing nothing but note and forward;
- the evaluate's inputs noted while armed, before the call;
- arming and the note-only hooks only with NR on, and the note-only hooks only without a Streamline FG input (the
  gate's whole expression);
- detach matching attach, with no `slSetConstants` detach on the note-only path;
- the line said after the D3D12 status gate and on the Vulkan after-upscale route;
- the design record.

Twelve source mutants must each fail a guard.

Not proved here:

- that the hooks compile with MSVC (the Release build covers that);
- that a game's tags arrive as assumed;
- that a tagged buffer holds what its type says;
- any frame-token or resource-state readability at the game's Present.
