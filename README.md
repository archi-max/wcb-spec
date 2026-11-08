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
| `src/wcb.h` | Current reference code, hardened reader (md5 `3ccaf977`). |
| `src/wcbfast.h` | Fast encoder, byte-identical output by default. |
| `src/split.h`, `src/dom.h` | Study codecs: changed-field structs, shared clock-domain timestamps (spec §5). |
| `src/variants/` | Other wcb.h versions; all write WCB2. `wcb.prehard.h` / `wcbfast.prehard.h` are the versions before the reader hardening. See spec §11. |
| `tests/` | `bench.cpp` (old vs fast encoder: bytes, round trip, timing), `tvalid.cpp` (malformed-input rejection), `tbound.cpp` (same-time block-boundary windows), `tcf.cpp` and `tdom.cpp` (study-codec round trips). |
| `tools/layout_check.cpp` | Struct sizes/offsets + 40K-change 4-state round trip. |
| `tools/wcbcheck.cpp` | Whole-file validator: open, decode every block and LOD chunk, recheck n / t0 / tlast / stream hash. `-DWCB_H='"path/wcb.h"'` picks the reader version. |
| `docs/wcb-vs-fsdb.md` | WCB vs FSDB summary: capture, encoder CPU, size, reads, checkpoints, and where WCB wins, ties and loses. |
| `docs/measurements.md` | Fast-encoder results and review fixes (Linux x86-64 host, 234 streams). |

## Build and check

```
F="-std=c++17 -lzstd"            # macOS/Homebrew: add -I/opt/homebrew/include -L/opt/homebrew/lib
g++ -O1 -fsanitize=address,undefined tests/tvalid.cpp $F -o tvalid && ./tvalid
g++ -O1 -fsanitize=address,undefined tests/tbound.cpp $F -o tbound && ./tbound
g++ -O1 -fsanitize=address,undefined tests/tcf.cpp $F -o tcf && ./tcf
g++ -O1 -fsanitize=address,undefined tests/tdom.cpp $F -o tdom && ./tdom
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
| `tcf` | PASS (300 random structs, 0 failures) |
| `tdom` | PASS (6,000 cases, 0 failures) |
| `layout_check` | Hdr 72 / BlkEnt 32 / LevEnt 16 / LodEnt 32; round trip OK |
| `wcbcheck` with the current, pre-hardening, base and `fold` readers | 234/234 ok, 4,185,535 changes, hashes match |
| `bench`, zstd-9 and zstd-3 | byte-identical 234/234, exit 0 |

## Reader hardening (2026-11-07)

`src/wcb.h` (md5 `3ccaf977`) and `src/wcbfast.h` (`bd2d9a7a`) replace `aa7b2a00` / `1e3dc535`, which are kept in `src/variants/` as `*.prehard.h`. The file format is unchanged, so existing `.wcb` files read as before. Encoder output is byte-identical on 234/234 streams.

- **Reader:** strict LEB128 varints, full header and index validation in `View::open` with overflow-safe bounds and size budgets, exact frame and payload sizes, time-order and overflow checks, and a validating `decodeLod`. Spec §4.1 has the rules.
- **Encoder:** the `wcbfast` per-thread workspace, the fmix64 DICT-slot fix and the LOD shift are folded into `wcb.h`.
- **Verified on the Linux host:** 43 crafted cases pass under ASan + UBSan and Valgrind; the pre-hardening reader fails them. Random-mutation fuzzing under ASan ran 6 targets × 1,800 CPU-s (7.7 M iterations) with 0 memory errors and 0 real violations. A first pass reported about 1,100 "violations"; a re-run showed that every one was a valid empty file (`nblk = 0`, where `blockAt`/`blockFirst` return 0). All 6,998 real stores tested open with no rejects, and hashes and counts match.
- **Re-verified on this Mac** (the table above): `tvalid` and `tbound` pass under ASan + UBSan, `wcbcheck` reads all 234 files, and `bench` is byte-identical 234/234 at zstd-9 and zstd-3.
- **Cost:** in the final interleaved run, block decode is 6–10% slower per value (corpus 0.0224 → 0.0238 µs/value), and LOD items 29–35% slower. A profile puts most of it in the XOR row loop, which the compiler no longer vectorizes; a fast path is in progress. That is still about 150× below FSDB's warm reader (3.56 µs/value; different sample set).
- **API note:** on an empty stream (`nblk = 0`), `blockAt`/`blockFirst` return 0. Check `nblk` before decoding; `decodeBlock` rejects an index ≥ `nblk`.

## Open items

- Prefix decode and a decoded-block cache in `wcb.h` (measured in a study reader; spec §4).
- Verify the bit-position order with a VPI writer on `4'b10xz`, for both `[3:0]` and `[0:3]` (spec §2).
- WCB3: container/metadata, streaming finish, per-block checksum and t_last, LSB-first flag (spec §9).
