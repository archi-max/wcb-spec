# WCB: per-signal lossless waveform store

**Start with [`WCB_FORMAT.md`](WCB_FORMAT.md)**, the WCB2 spec. It covers:
- byte layout and value packing (4-state planes, bit order, VPI mapping);
- the XOR/DICT block encoding and the LOD index;
- reader validation, integrity, and the proposed container;
- measured numbers and the WCB3 gap list.

For the head-to-head numbers against FSDB, see [`docs/wcb-vs-fsdb.md`](docs/wcb-vs-fsdb.md).

This spec is a revision of the project's earlier working draft. It adds byte-offset tables, exact LOD rules and the reader-validation rule set, updates the §9 status, and fixes source pointers.

## Layout

| Path | What it is |
|---|---|
| `WCB_FORMAT.md` | The spec (revised). |
| `src/wcb.h` | Current reference code (md5 `aa7b2a00`). |
| `src/wcbfast.h` | Fast encoder, byte-identical output by default. |
| `src/split.h`, `src/dom.h` | Study codecs: changed-field structs, shared clock-domain timestamps (spec §5). |
| `src/variants/` | Other wcb.h versions; all write WCB2. `wcb.fold.h` has the strictest reader (spec §4.1). See spec §11. |
| `tests/` | `bench.cpp` (old vs fast encoder: bytes, round trip, timing), `tvalid.cpp` (malformed-input rejection), `tbound.cpp` (same-time block-boundary windows). |
| `tools/layout_check.cpp` | Struct sizes/offsets + 40K-change 4-state round trip. |
| `tools/wcbcheck.cpp` | Whole-file validator: open, decode every block and LOD chunk, recheck n / t0 / tlast / stream hash. `-DWCB_H='"path/wcb.h"'` picks the reader version. |
| `docs/wcb-vs-fsdb.md` | WCB vs FSDB summary: capture, encoder CPU, size, reads, checkpoints, and where WCB wins, ties and loses. |
| `docs/measurements.md` | Fast-encoder results and review fixes (Linux x86-64 host, 234 streams). |

## Build and check

```
F="-std=c++17 -lzstd"            # macOS/Homebrew: add -I/opt/homebrew/include -L/opt/homebrew/lib
g++ -O1 -fsanitize=address,undefined tests/tvalid.cpp $F -o tvalid && ./tvalid
g++ -O1 -fsanitize=address,undefined tests/tbound.cpp $F -o tbound && ./tbound
g++ -O2 tools/layout_check.cpp $F -o layout_check && ./layout_check
g++ -O2 tools/wcbcheck.cpp $F -o wcbcheck && ./wcbcheck path/to/*.wcb
g++ -O3 tests/bench.cpp $F -o bench && ./bench 9 16384 1 3 path/to/*.wcb
```

(Set `ASAN_OPTIONS=detect_leaks=0`; the tests never free their ZSTD_DCtx.)

Last run, 2026-11-07, Apple M4 Pro, zstd 1.5.7, on a 234-file corpus of WCB2 streams converted from synthetic-design FSDB dumps (not included in this repo). All of these pass:

| Check | Result |
|---|---|
| `tvalid` | PASS |
| `tbound` | PASS (153/153 windows) |
| `layout_check` | Hdr 72 / BlkEnt 32 / LevEnt 16 / LodEnt 32; round trip OK |
| `wcbcheck` with the base, current and `fold` readers | 234/234 ok, 4,185,535 changes, hashes match |
| `bench`, zstd-9 and zstd-3 | byte-identical 234/234, exit 0 |

## Open items

- Merge `wcb.fold.h`'s strict reader into `src/wcb.h` (spec §9 item 9).
- Verify the bit-position order with a VPI writer on `4'b10xz`, for both `[3:0]` and `[0:3]` (spec §2).
- WCB3: container/metadata, streaming finish, per-block checksum and t_last, LSB-first flag (spec §9).
