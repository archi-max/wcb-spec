# WCB waveform format (study version WCB2)

This document specifies the custom waveform format used by our waveform-store research. It is written so that a writer can be built directly from VCS through VPI (a VPI plugin that captures value changes and hands them to a custom serializer), with no FSDB written first.

It describes the format **as implemented in `src/wcb.h`** (magic `WCB2`, md5 `aa7b2a00`), plus two optional study codecs (`split.h` changed-field coding, `dom.h` shared clock-domain timestamps). Every statement carries one of these labels:

- **Implemented:** in `wcb.h` today. Round trips verified bit-exact (hash per signal) on 1.78 G changes from real FSDB dumps.
- **Study codec:** the code exists and is verified, but there is no on-disk container for it yet.
- **Proposed:** designed, not implemented.

Status: research format. The cmux app doesn't use it yet; it still reads FSDB and caches viewed signals in a separate `.vvz` deflate cache. **WCB2 is a compact backend for digital RTL value changes. It is not yet a complete FSDB replacement.** Expect a version bump (WCB3) before production; §9 lists what is missing.

**Scope of the headline numbers.**
- **Decode, "~40×":** this is **decode CPU per value** on one workload: FSDB's warm reader at 3.56 µs vs `decodeBlock` at 0.082 µs (43×). It says nothing about dumping speed.
- **Encode, old `wcb.h`** (base, md5 `15808d70`, kept as `src/variants/wcb.base.h`): 1.68 µs/change, **~2× slower** than FSDB's writer (0.85).
- **Encode, current `wcb.h` and `wcbfast.h`** (same WCB2 output, byte-identical on 234/234 streams; DICT always tried, LOD on, init included). Figures are µs/change on a shared Linux x86-64 host:

  | | zstd-9 | zstd-3 | zstd-9 wide (>64-bit) | zstd-3 wide |
  |---|---:|---:|---:|---:|
  | `wcbfast.h` | 0.46 | 0.25 | 0.62 | 0.32 |
  | current `wcb.h` | 0.49 | 0.28 | | |

  This is encoder CPU on already-decoded streams. **VPI capture is not included.** Beating FSDB's writer (0.80–0.85) still has to be shown on the same design, signals, semantics and host, with capture included.

> **About this revision (2026-11-07).** It is based on the project's earlier working draft of this spec. Changes:
> - byte-offset tables for every struct (§3), checked by `tools/layout_check.cpp`;
> - the exact LOD level-count rule (it uses the writer's `tmax` hint, not `tlast`) and the invalid LOD byte values (§3.4);
> - a reader-validation section (§4.1), taken from the newest reader (`src/variants/wcb.fold.h`);
> - §9 status updated: the window-start fix, the CCtx leak, encoder speed and most reader validation are now **done in code**;
> - stale source pointers fixed (§11).
>
> Nothing about the format itself changed. Re-verified on this Mac: the old, current and `fold` readers each decode all 234 corpus files with matching stream hashes (`tools/wcbcheck`). `tvalid` and `tbound` pass under ASan + UBSan, and `bench` is byte-identical 234/234.

---

## 1. Design goals

1. **Lossless, 4-state exact.** Every change, in order, including X and Z, and including multiple changes at the same time.
2. **Cheap random access.** Finding the value at time t costs one binary search plus one block decode, independent of file size.
3. **Cheap zoomed-out drawing.** Precomputed level-of-detail (LOD) summaries, so a 10 GB dump can be drawn zoomed out without decoding every change.
4. **Cheap to write.** A streaming encoder per signal; no global pass needed. Measured 0.2–0.5 µs per change (§8).
5. **Simple to decode.** varint + XOR + zstd. A non-validating decoder is about 30 lines.

## 2. Value representation (4-state packing)

Each change carries one value of `vlen` bytes.

**4-state signals (`fourState = 1`):** two bit planes of `nb = ceil(bits/8)` bytes each, so `vlen = 2 * nb`.

```
bytes [0 .. nb)      value plane  (a)
bytes [nb .. 2*nb)   x/z plane    (b)
bit position i  ->  byte (i >> 3), bit (i & 7) of each plane
unused high bits of the last byte of each plane are 0
```

| Logic value | value plane a | x/z plane b |
|---|---|---|
| 0 | 0 | 0 |
| 1 | 1 | 0 |
| X | 0 | 1 |
| Z | 1 | 1 |

**2-state signals (`fourState = 0`):** one plane of `vlen = nb` bytes, with no x/z plane.

A reader must check `vlen == (fourState ? 2 : 1) * ceil(bits/8)`.

**Bit position order.** Position `i` is the i-th bit in the order the FSDB reader returns a value (one byte per bit, index 0 first). `split.h` documents that order as MSB-first: **position 0 is the declared MSB, not bit 0 of the vector.** A writer must reproduce this exact order. Before trusting a new writer, verify the order against a known value, e.g. `4'b10xz`, and check both a `[3:0]` and a `[0:3]` declaration (§9).

**From VPI (`vpi_get_value`, `vpiVectorVal`).** VPI's encoding differs: Z is aval=0, bval=1, and X is aval=1, bval=1. Convert to WCB planes word-wise:

```
wcb value plane  a = aval XOR bval
wcb x/z plane    b = bval
```

That handles 0/1/X/Z correctly. VPI numbers bit 0 as the vector's LSB, so with the current MSB-first position order a VPI writer must also reverse bit positions (§9, item 1).

## 3. File layout (implemented)

There is one file per signal. All integers are little-endian. The structs are written exactly as laid out in memory (natural alignment, x86-64 and arm64 little-endian). Sizes and offsets below are checked by `tools/layout_check.cpp`.

```
offset 0
[Hdr]                      72 bytes
[BlkEnt  x Hdr.nblk]       32 bytes each   block index, time order
[LevEnt  x Hdr.nlev]       16 bytes each   LOD level table, level 0 first
[LodEnt  x Hdr.nchunk]     32 bytes each   LOD chunk index, all levels in level order
                                           <- idx = end of index
[block data]               zstd frames, one per block
[LOD data]                 zstd frames, one per LOD chunk, level by level
```

The header and indexes are small and contiguous, so a reader can `mmap` the file and use them in place. `BlkEnt.off` and `LodEnt.off` are **absolute file offsets**. The writer packs frames with no gaps, but readers must use the offsets, not assume packing.

**varint:** unsigned LEB128, 7 bits per byte, low bits first, high bit = continue. At most 10 bytes. Writers emit only the shortest (canonical) form.

### 3.1 Hdr (72 bytes)

| Off | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u32 | `magic` | `0x32424357` = bytes `57 43 42 32` = `"WCB2"`. `0x31424357` `"WCB1"` = older LOD entry format, still readable (§3.4). Any other value is a hard error. |
| 4 | u32 | `bits` | Declared width in bits, ≥ 1. |
| 8 | u32 | `vlen` | Bytes per value (§2). |
| 12 | u32 | `fourState` | 1 = value + x/z planes, 0 = one plane. No other values. |
| 16 | u64 | `n` | Total number of changes. |
| 24 | u64 | `t0` | Time of the first change (0 if `n == 0`). |
| 32 | u64 | `tlast` | Time of the last change (0 if `n == 0`). |
| 40 | u64 | `lodBase` | LOD level-0 bucket width in time units. 0 = no LOD (then `nlev == 0`). |
| 48 | u64 | `hash` | Chained hash over every (t, full vlen value), §6. |
| 56 | u32 | `nblk` | Number of value blocks. 0 if and only if `n == 0`: a zero-change signal is a header-only file. |
| 60 | u32 | `nlev` | Number of LOD levels. |
| 64 | u32 | `nchunk` | Total LOD chunks over all levels. |
| 68 | u32 | `pad` | 0. |

Times are unsigned 64-bit counts of the dump's time unit. **The time unit (timescale) is not stored in the file**, so it must come from a container or manifest (§7).

### 3.2 BlkEnt: value-block index (32 bytes)

| Off | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u64 | `t0` | Time of the block's first change (absolute). Non-decreasing across blocks. |
| 8 | u64 | `off` | File offset of the block's zstd frame. |
| 16 | u32 | `csize` | Compressed size. |
| 20 | u32 | `n` | Changes in the block, `1 .. blkN`. |
| 24 | u32 | `rsize` | Decompressed payload size. |
| 28 | u8 | `mode` | 0 = `M_XOR`, 1 = `M_DICT`. Anything else is invalid. |
| 29 | u8 | `flags` | bit 0 `F_2ST`: this block has no X/Z, so its x/z plane is omitted. Other bits are 0; `F_2ST` is legal only when `fourState = 1`. |
| 30 | u8[2] | pad | 0. |

Blocks are in time order, and each decodes alone. The default `blkN` is 16,384 changes per block. 4,096 was also measured: faster point queries, about 39% larger.

### 3.3 Value-block payload (after zstd decompression)

```
varint dt[n]        dt[0] = 0 (the block's t0 is in BlkEnt); time[i] = time[i-1] + dt[i]
values              by mode, below
```

`w` = bytes stored per value: `vlen` normally, `vlen/2` if `F_2ST` is set. In the `F_2ST` case the decoder fills the x/z plane with zeros. The writer sets `F_2ST` on any block of a 4-state signal where no change has an x/z bit set, so an X during reset only costs space in the blocks that contain it.

- **M_XOR:** n records of w bytes. Record i = value[i] XOR value[i-1], with value[-1] = all zeros, so the first value is stored as is. The value section is exactly `n * w` bytes.
- **M_DICT:** per change, a varint `k`:
  - `k = 0`: w bytes follow, a literal XOR of the previous value as in M_XOR. This value becomes the next dictionary entry.
  - `k >= 1`: the value equals the k-th distinct literal value seen so far in this block (1-based, first-occurrence order). No bytes follow. `k` larger than the number of entries so far is invalid.

  In both cases the "previous value" for the next change is the value just decoded, whether it was a literal or a repeat. The dictionary starts empty in every block.

The encoder builds both payloads for every block and keeps the smaller after zstd; on a tie it keeps M_XOR. This is writer policy: readers must accept either mode on any block. (`wcbfast.h` `Policy::SkipNoRepeat` and `wcb.h` `gateDict` are opt-in shortcuts that skip DICT and make files 0.01–0.2% larger.) zstd level: 3 when writing live, 9 as the default, 19 for an optional background recompress. Frame checksums are off.

**Same-time changes:** dt = 0 is legal. Multiple changes at one timestamp are kept in order, so glitches are preserved.

### 3.4 LOD: precomputed zoomed-out summaries

Level L has bucket width `lodBase * 4^L` (`lodBase << 2L`). A change at time t is in bucket `floor(t / (lodBase << 2L))`, which equals `floor(t / lodBase) >> 2L`.

**Level count (writer rule; readers just use `nlev`).** `Encoder::init` takes a `tmax` hint (the expected run end), which is not stored in the file. It picks the smallest `m ≤ 24` with `lodBase << 2m ≥ tmax / 1024`, then sets `nlev = m + 1`. The coarsest level therefore has about 1,024 or fewer buckets over the run.

Each level is a **sparse** list of the buckets that hold at least one change, in increasing bucket order, cut into chunks of up to 16,384 entries.

| `LevEnt` (16 B) | Off | Type | Meaning |
|---|---:|---|---|
| `firstChunk` | 0 | u32 | Index of the level's first `LodEnt`. Levels' chunk ranges are contiguous and in level order. |
| `nchunk` | 4 | u32 | Chunks in this level. |
| `entries` | 8 | u64 | Total entries (non-empty buckets) in the level. |

| `LodEnt` (32 B) | Off | Type | Meaning |
|---|---:|---|---|
| `b0` | 0 | u64 | Bucket index of the chunk's first entry. |
| `off` | 8 | u64 | File offset of the chunk's zstd frame. |
| `csize` | 16 | u32 | Compressed size. |
| `n` | 20 | u32 | Entries in the chunk, ≥ 1. |
| `rsize` | 24 | u32 | Decompressed size. |
| pad | 28 | u32 | 0. |

A chunk payload (WCB2, after zstd) holds `n` entries:

```
varint bucketDelta     first entry: 0 (its index is b0); then index - previous index (>= 1)
uint8  flags           bits 0-1: min(changes in bucket, 2)   (drawing only needs 1 / many; 0 and 3 are invalid)
                       bit 2:    any X/Z in the bucket
                       bit 3:    1-bit signals only: value-plane bit of the last change in the bucket
                       bits 4-7: 0
```

WCB1 used `varint delta, varint count, uint8 flags` per entry instead, with an exact count. Readers accept both.

Recommended `lodBase`: 32 clock cycles. At an 8-cycle base the LOD was about 45% of block bytes for typical primaries; at 32 cycles it was 5.5%.

## 4. Reading

- **Value at time t:**
  1. `blockAt(t)` binary-searches `BlkEnt.t0` for the last block with t0 ≤ t.
  2. Decode that block and take the last change with time ≤ t.
  3. Before `Hdr.t0` there is no recorded value; the first change is the initial value.
- **Window [t1, t2) (inclusive change scans):**
  - Starting at `blockAt(t1)` is **wrong** when changes at the same timestamp span a block boundary. If block k ends with changes at time T and block k+1 starts at t0 = T, `blockAt(T)` returns k+1, and block k's changes at T are skipped. Reproduced: blkN = 4, times 10, 20, 30, 50 | 50, 50, 60, 70; window [50, 61) returned 3 changes instead of 4.
  - **Correct start (implemented as `View::blockFirst`):** j = the first block with t0 ≥ t1 (lower_bound), start at max(j − 1, 0), and read through the last block with t0 < t2. Block j − 1 also holds the value in effect at t1. `tests/tbound.cpp` checks this, plus 153 collapsed windows against brute force.
  - The same applies to any "changes from t" or edge search. WCB3 should store `t_last` per block (§9) so this can be bounded without decoding.
- **Prefix decode (measured optimisation, not in `wcb.h`):** a point query only needs changes up to t, so decoding can stop early. Measured: 20 signals at a point, 0.64 ms vs 1.94 ms for a warm FSDB reader.
- **Zoomed out:** pick the level whose bucket width is about one pixel, binary-search `LodEnt.b0` within that level for the chunks overlapping the view, decode them, and draw from the flags. A bucket with no entry had no change.

### 4.1 Reader validation

A reader may be given truncated, corrupt or hostile files, so it must reject them without reading out of bounds. Failure is atomic: a block that fails returns nothing, never a partial result.

**Current `wcb.h` (implemented):**
- `View::open` checks header and index bounds against the file size.
- `decodeBlock` returns false on a zstd error, a decompressed size ≠ `rsize`, `mode > 1`, a varint or payload overrun (varints bounded to 10 bytes), a DICT index out of range, or `off + csize` past the end of the file.
- `tests/tvalid.cpp`: 6/6 crafted faults are rejected. Of 3,000 random bit flips, about 2/3 are rejected and the rest decode to other values (there is no checksum, §6), with no crash.

**Full rule set (implemented in `src/variants/wcb.fold.h`, the newest reader; candidate for WCB2's normative reader).**
- It decodes all 234 corpus files.
- It was fuzzed under ASan on the Linux host (from that run's output; not re-run for this revision). 250,000 iterations over 74 seeds mutated raw files, index fields and payloads. Of these, 1,584,093 blocks and 410,684 LOD chunks decoded, with **0 invariant violations**.

At `View::open`, the whole index is checked in O(nblk + nchunk):
- the file is at least 72 bytes;
- magic is WCB1 or WCB2;
- `fourState ∈ {0,1}`, `bits ≥ 1`, and `vlen` matches §2 with `vlen ≤ 2^20`;
- `nlev ≤ 64`;
- the index fits in the file;
- `(n == 0) == (nblk == 0)`.

Per block (`BlkEnt`):
- `mode ≤ 1`;
- `flags ⊆ {F_2ST}`, and `F_2ST` only if `fourState`;
- `1 ≤ n ≤ 2^24`;
- `idx ≤ off` and `off + csize ≤ file size`;
- `rsize ≥ n`, `n · vlen ≤ 2^30` and `rsize ≤ 2^30`;
- `t0` is non-decreasing.

Whole file, then LOD:
- Σ `BlkEnt.n` = `Hdr.n`;
- `BlkEnt[0].t0 = Hdr.t0`, and the last block's `t0 ≤ tlast`;
- level chunk ranges are contiguous and sum to `nchunk`;
- `nlev > 0` requires `lodBase > 0`;
- each chunk has `n ≥ 1`, is in bounds, and has `rsize ≥ 2n` (WCB1: `3n`).

When decoding a block or chunk:
- if the zstd frame declares a content size, it must equal `rsize`, and decompression must yield exactly `rsize` bytes;
- varints must be canonical (no overlong forms, no 10th byte above 1), with no u64 overflow when summing times or buckets;
- `dt[0] = 0`, and the last time is ≤ `Hdr.tlast`;
- M_XOR: the value section is exactly `n·w` bytes;
- M_DICT: every literal must fit and every `k` must be in range;
- LOD: the first delta is 0 (WCB2) and later deltas are ≥ 1; count bits ∈ {1, 2}; flag bits 4–7 are 0;
- no trailing bytes.

## 5. Optional study codecs (verified; no container yet)

### 5.1 Changed-field coding for packed structs (`split.h`, `CFEnc`)

This is for wide structs where only a few fields change per event. Fields are `[pos, width)` ranges in the bit-position space of §2, at most 64 bits each, taken from the design's struct declaration.

Block layout, 16K changes, zstd, using `BlkEnt` with `mode` unused:

```
varint dt[n]
masks      n records of ceil(nfields/8) bytes; bit f = field f changed in this change
           (the first change of a block marks every field)
for each field f: its value-plane XOR deltas, ceil(w_f/8) bytes LE, one per change where bit f is set
if 4-state and the block has X/Z: the same again for the x/z plane
```

Field state resets at each block start, so blocks decode alone.

- **Measured** (8 × 512-bit structs, the same values as an FSDB narrow dump): **0.539 B/change against FSDB's 1.344**. Encode 0.52 µs/change, decode 0.080 µs/value.
- **Use adaptively:** on busy structs where most fields change every event, CF can be larger than plain XOR. Use CF only where it saves more than ~25%.
- **Not yet defined:** where the field schema is stored (§7).

### 5.2 Shared timestamps per clock domain (`dom.h`)

Signals clocked by the same edge share one edge-time list, stored once (varint deltas, zstd, in chunks of 16,384 edges). A member signal's block keeps its value payload, but its time header becomes one of:

- `TK_IDX`: per change, `varint(edgeIndexDelta << 1)` for a change on an edge, or `varint((dt << 1) | 1)` as an escape for a change off the edge list;
- `TK_BITMAP`: a bitmap over the edges the block spans, plus an escape list, when that is smaller;
- `TK_DT`: plain, as in §3.3.

The block index keeps the absolute t0, so random access is unchanged. **Measured:** −12.6 to −16.6% on primaries.

## 6. Integrity

- `Hdr.hash` is a chained hash. It starts at `h = 1469598103934665603` (the FNV-1a offset basis), and for every change `h = hmix(h, t, value, vlen)`. It covers the full vlen value, including a zero-filled x/z plane:

  ```c
  uint64_t hmix(uint64_t h, uint64_t t, const uint8_t *v, size_t n) {
    uint64_t a = 0x9E3779B97F4A7C15 ^ n; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w = load_le64(v + i); a ^= w; a *= 0xff51afd7ed558ccd; a ^= a >> 32; }
    for (; i < n; i++)         { a ^= v[i];  a *= 0x100000001b3; }
    h ^= a ^ (t * 0xC2B2AE3D27D4EB4F); h *= 0x100000001b3; h ^= h >> 29;
    return h;
  }
  ```

  A reader that decodes the whole stream can verify it (`tools/wcbcheck`). It is not cryptographic.
- The encoder recomputes the hash from its own decode and compares it with the source stream's. A mismatch means the file must not be used.
- **No per-block checksum** in WCB2 (§9). A flipped bit inside a compressed frame can decode to different values without any error.
- An unknown `magic` is a hard error; never guess.

**What has actually been verified (scope):**
- **Per-signal conversion round trips** (the 1.78 G-change figure): an ordered hash over every (t, value) change of one signal, in the order the FSDB reader walked them. That is event-exact within a signal, including same-time changes in order. It says nothing about ordering **between** signals (§9).
- **The fair benchmark against the FSDB reader** (`wcnav.cpp` / `wcfair.cpp`) compares **collapsed** results: the value at a, then the changes in (a, b], with the last value winning at a timestamp and equal consecutive values dropped. Its matching hashes show the drawn waveform is the same. They do **not** show glitch- or event-order equivalence.
- **The computed-signal generator gate** (1,947/1,947): check its comparison semantics the same way before calling it event-exact.

## 7. Container and manifest (proposed: "WCZ")

`wcb.h` defines only the per-signal file. Today the signal dictionary lives outside it, in the converter's ids and manifest files. A complete format, and especially a VPI writer, needs a container. Proposed design:

```
FileHeader   magic "WCZ1", container version, source stamp, timescale, tmax, lodBase,
             codec-registry version, n_signals, manifest offset
Manifest     append-only records: {signal_id, record_off, record_len, hash, state complete|partial}
SignalRecord per stored signal: Hdr-equivalent + BlkEnt[] + LOD index (as §3), with a
             codec_id + entropy_id per block (registry lookup; an unknown id is a hard error naming the codec)
Dictionary   hierarchy: scopes, names, declared ranges [msb:lsb], kind (net/reg/port dir/param),
             type (logic/bit/int/enum + enum table/struct + field schema), and aliases -> signal_id
```

Whole-signal codecs, stored as a pointer or parameters instead of blocks:
- **REF:** an identical stream (dedupe). This is the biggest single win: about 42% of bytes on our benchmark design were exact copies.
- **SHIFT:** another signal shifted by a constant time.
- **PERIODIC:** clocks, stored as a period plus exceptions.

## 8. Measured numbers (synthetic SoC benchmark design; one design)

**Same 8 × 512-bit struct signals, same host (Xeon 4216, 2.1 GHz):**

| | Bytes/change | Encode µs/change | Decode µs/value |
|---|---|---|---|
| FSDB (narrow-dump delta; warm reader with navigation loaded once) | 1.344 | 0.85 (writer threads 0.55) | 3.56 |
| WCB2 XOR/DICT, zstd-9, times included | 0.944 | 1.68 old `wcb.h`; 0.19–0.26 optimised XOR-only loop | 0.082 (`decodeBlock`); 0.022 optimised |
| CF (§5.1), zstd-9 | 0.539 | 0.52 | 0.080 |

**Encoder, 234-stream corpus (4.19 M changes, 19 zero-change).** Init included, best of 3, µs/change; full tables in `docs/measurements.md`:

| Host | Encoder | zstd-9 | zstd-3 | Output |
|---|---|---:|---:|---|
| Linux x86-64 | old `wcb.h` | 1.22 | 0.93 | baseline |
| Linux x86-64 | `wcbfast.h` Preserve | 0.46 | 0.25 | byte-identical 234/234 |
| Linux x86-64 | current `wcb.h` | 0.49 | 0.28 | byte-identical |
| Apple M4 Pro, zstd 1.5.7 (2026-11-07, this repo) | current `wcb.h` / `wcbfast.h` | 0.190 / 0.185 | 0.069 / 0.063 | byte-identical 234/234 |

The old-vs-fast ratio moves with host load and run order (2–4×). Quote ranges, not single numbers, and compare against the current `wcb.h`, not the old one. **Byte totals depend on the zstd library version.** At zstd-3 the corpus is 3,700,970 B with zstd 1.5.5 (Linux host) and 3,700,395 B with 1.5.7 (Mac). Compare files only when they were written with the same libzstd; the format and the decoded values don't change.

**DICT hash-slot fix** (fmix64 before masking; output unchanged): 100 mixed-width counters at zstd-3 went from 0.393 to 0.089 µs/change with `wcb.h` on the Linux host. On matched VCS runs, a separate prototype of the same fix measured mixed-counter CPU of 2.33 s → 0.88 s, against native FSDB's 1.09 s, with WCB 73.5% smaller.

**Whole stored set of a 1.72 GB FSDB:** 804 MB of WCB2 blocks at zstd-3 after dedupe, about 2.1× smaller. Projected (not measured) with CF and shared timestamps at zstd-9: about 490–510 MB, or 560–580 MB with LOD.

These numbers come from one synthetic design. Real-design (CVA6) measurements are in progress.

## 9. Known gaps; changes planned for WCB3

**Missing for a self-contained, native-capture format** (the external review's list, all agreed):

| Area | Gap | Status 2026-11-07 |
|---|---|---|
| Self-contained metadata | Timescale, hierarchy, names, ranges, signedness, types, aliases and run duration are external or only proposed (§7). A native capture is uninterpretable without them. | open |
| Dump-off intervals | There is no way to tell "unchanged" from "not recorded". Add explicit dump-off/dump-on ranges (FSDB exposes them). | open |
| Same-time ordering across signals | Per-signal streams keep their own order but can't rebuild the interleaving between signals at one timestamp. If delta-cycle or event-order views need it, add a global sequence number per change or per timestamp group, optionally, as FSDB does. | open |
| Streaming and recovery | `Encoder` keeps all compressed data in memory until `finish()`. Native capture needs bounded memory, incremental writes, and finalization that can be recovered after a crash (item 6 below). | open |
| Other data types | Fixed-width logic planes don't cover real/string values, strength, named events, dynamic objects or transactions. Exclude these explicitly from an initial "RTL profile" and reject them at the writer, rather than dropping them silently. | open |
| Reader validation | The original `decodeBlock`/`View` didn't bound offsets, sizes, varints or DICT indexes, ignored zstd return codes, and treated any mode ≠ 0 as DICT. | **done in current `wcb.h`** (§4.1); the stricter `fold` rules are not yet merged |
| Window queries | The block-boundary bug in §4. | **fixed** (`blockFirst`, `tbound` passes) |

**Performance rule for every WCB3 change: never worse than FSDB.** Measure each addition on the same samples (the 8-struct same-sample set, plus a whole-file run) for size, encode, decode, and point/window query latency, against FSDB on the same host. It must stay at or better than FSDB on **all four**; if it can't, it is optional and off by default.

| Change | Expected cost (ESTIMATE until measured) | Keeps us ahead of FSDB? |
|---|---|---|
| Window-start fix | at most +1 block decode per window start; 0 with per-block t_last | yes (done) |
| Per-block t_last | +8 B per 16K-change block (~0.0005 B/change) | yes |
| Per-block checksum (xxh64 of the frame) | ~0.1 ns/byte at decode, under 1% of decode | yes |
| Reader validation (bounds, return codes, varint and DICT checks) | a few % of decode | yes (done; decode is ~43× ahead) |
| Self-contained metadata, dump-off table | once per file, ~0 per change | yes |
| Streaming flush with bounded memory | lower peak memory; small extra I/O | yes, if each block is written once |
| LSB-first flag | removes the per-value bit reversal for VPI writers | faster |
| **Cross-signal order (sequence numbers)** | 1–3 B/change before zstd if stored on every change, which could erase much of the size lead | **only as an option**: store sequence data only where ≥ 2 signals change at one timestamp, or only when event-order fidelity is requested |
| **Encoder speed** | old `wcb.h` was ~2× slower than FSDB | **done for encoder CPU:** 0.46/0.25 µs at zstd-9/3 (§8). Still needs an end-to-end comparison with FSDB's writer, capture included, on a matched workload |

**Implementation changes:**

1. **Bit position order:** define it explicitly, with a header flag for LSB-first, so a VPI writer can store `aval`/`bval` words without reversing bits. *Open.*
2. **Timescale and dictionary:** move them into the container (§7). A bare `.wcb` can't be interpreted without them. *Open.*
3. **Endianness and portability:** structs are memcpy'd in host layout. WCB3 should define explicit field serialisation, or at least an endianness marker. *Open.*
4. **Per-block checksum** (e.g. xxh64 of the compressed frame), so corruption fails one block instead of passing silently. *Open.*
5. **BlkEnt t_last** per block, to bound window searches without decoding. *Open.*
6. **Streaming finish:** a VPI writer needs time-interval flushes (e.g. every 1% of the run) and an append-only layout, with the index written at close and recoverable after a crash. Peak buffer memory for many signals must be measured. *Open.* (Measured today: about 9.7 MiB of shared workspace per thread; the busiest wide signal's encoder holds 2.3 MiB.)
7. **Encoder ZSTD_CCtx leak** (~4 MB per encoder). *Fixed:* one context per thread.
8. **Encoder speed:** byte-by-byte candidate building. *Fixed:* a single-pass, word-wise builder, byte-identical output.
9. **Variant drift:** the `fold` reader (strict varints, full index validation) and the `wcb.h` in `src/` have diverged. Merge them into one header before WCB3 work starts. *Open.*

## 10. Writing from VPI: mapping to a VPI capture hook

| Capture-hook requirement | WCB answer |
|---|---|
| Signal IDs, names, widths, ranges, types, aliases | `bits` and `fourState` per file; names, ranges, types and aliases go in the container dictionary (§7) |
| Time precision and representation | u64 ticks, delta-coded per block; the timescale goes in the container header |
| 4-state values and bit order | §2: `a = aval ^ bval`, `b = bval`; see §9 item 1 for bit order |
| Multiple changes at one time | kept in order within a signal (dt = 0 allowed); ordering **across** signals is not recorded (§9) |
| Buffering | **The current `Encoder` holds every compressed block in memory until `finish()`; nothing is written early.** Needed: write each full block to the file as it fills, with bounded memory (§9) |
| Compression | zstd-3 live; optional zstd-9/19 recompress later |
| Indexing | BlkEnt per block + LOD per level, written at close |
| Finalization and errors | indexes and header are written at close; the stream hash is computed incrementally. Crash recovery is not designed yet (§9 item 6) |

**Cost to watch:** VPI value-change callbacks are not free. On the benchmark design, watching 100 signals cut simulation speed from 9,532 to 6,979 cycles/s. Separate measurements: callback-supplied values cost about 0.10 µs per callback, and explicit value/time reads about 0.22. Measure a VPI writer against no dump and against native FSDB on the same signals.

## 11. Reference source (this repo)

| File | md5 (8) | What |
|---|---|---|
| `src/wcb.h` | `aa7b2a00` | **Current.** Header, block index, LOD, `Encoder` (fast path, per-thread CCtx, fmix64 DICT slots), `View` (`blockAt`, `blockFirst`), validating `decodeBlock`/`decodeLod`.  |
| `src/wcbfast.h` | `1e3dc535` | **Current fast encoder** (latest: fmix64 DICT slots + LOD shift), same output; `Policy::Preserve` (default) or `SkipNoRepeat`. Earlier `wcbfast.h` copies without those two fixes (e.g. md5 `4ad906a8`) are obsolete. |
| `src/variants/wcb.fold.h` | `29189c4d` | Newest reader: the full §4.1 rules. Also experimental fold work. |
| `src/variants/wcb.shift.h` | `c7570e71` | `wcbfast` design merged into `wcb.h`, with the LOD shift trick. |
| `src/variants/wcb.fair.h` | `7c6299cb` | Earlier validating version (the md5 the earlier draft cited). |
| `src/variants/wcb.base.h` | `15808d70` | Original 160-line reference; the baseline "old `wcb.h`" timings. |
| `tests/` | | `bench.cpp`, `tvalid.cpp`, `tbound.cpp`  |
| `tools/` | | `layout_check.cpp` (struct sizes and round trip), `wcbcheck.cpp` (whole-file validator) |

| `src/split.h`, `src/dom.h` | | Study codecs (§5), They compile against the current `wcb.h`; they were not re-verified here. |
