# wcbfast: a faster WCB2 encoder (same file format, readers unchanged)

Revised 2026-11-07 after a code review (findings 1–6). All five code findings are fixed.

| Review finding | Fix |
|---|---|
| 1. The code default skipped DICT, contradicting the README | `Policy::Preserve` (always try DICT, keep the smaller) is the code default. `Policy::SkipNoRepeat` is opt-in. The wrong "DICT can't win without repeats" comment is removed; it can win on 512-bit structs. |
| 2. ~2–82 MiB of scratch per encoder | Scratch (both raw candidates, both compressed buffers, the hash table) and the ZSTD_CCtx are in ONE `thread_local Workspace` shared by every encoder on the thread. Each encoder holds only its open block (grown on demand, as in wcb.h) plus its compressed history. |
| 3. The timer excluded init | Init is included, and both numbers are reported. With lazy allocation they are now equal to 3 digits. |
| 4. Failures exited 0 | Nonzero exit on any round-trip failure, unreadable/skipped file, empty corpus, Preserve-mode byte mismatch, or blkBytes/lodBytes mismatch. Negative tests (`/etc/hostname`, a random 100-byte file) exit 1. |
| 5. Not API-compatible | Adds `blkBytes()`, `lodBytes()` (checked equal to wcb.h's on every stream), plus `stateBytes()` (per-encoder memory). Copyable and movable (no owned raw pointers); the bench exercises a move and a copy. |
| 6. The spec contradicted itself | WCB_FORMAT.md's intro and §9 now carry the same measured numbers and scope. |

## Results (Linux x86-64 host, g++ -O3, best of 3, LOD on, 234 real WCB2 streams incl. 19 zero-change, 4.19 M changes)

| Level | Policy | Bytes vs wcb.h | Identical | µs/change all (init incl.) | wide >64b |
|---|---|---:|---:|---:|---:|
| zstd-9 | wcb.h today | 3,068,507 | — | 1.221 | 1.830 |
| zstd-9 | **Preserve (default)** | **+0.000%** | **234/234** | **0.462 (2.6×)** | **0.621 (2.9×)** |
| zstd-9 | SkipNoRepeat (opt-in) | +0.044% | 195/234 | 0.342 (3.5×) | 0.428 |
| zstd-3 | wcb.h today | 3,700,970 | — | 0.933 | 1.411 |
| zstd-3 | **Preserve (default)** | **+0.000%** | **234/234** | **0.245 (3.8×)** | **0.315 (4.5×)** |
| zstd-3 | SkipNoRepeat (opt-in) | +0.208% | 196/234 | 0.204 (4.5×) | 0.249 |

All runs round-trip exactly (event-exact: times and values, per signal) and exit 0. LOD off at zstd-3 also passes.

Memory: one shared workspace per thread, 9.7 MiB (sized by the widest block seen). The largest per-signal state was 2.3 MiB, on the busiest wide signal: its open 16K-change block plus its retained compressed history. Idle and scalar signals hold only what they have buffered. Previously every encoder allocated 2–82 MiB of scratch up front.

## Scope (what this does NOT show)

- Encoder CPU on already-decoded streams. VPI capture cost is not included (separate measurements: callback-supplied values ~0.10 µs/callback, explicit value/time reads ~0.22).
- Beating FSDB's native writer (0.80–0.85 µs/change) end to end needs the same design, signals, semantics and host, capture included. That is not yet measured.
- It still retains all compressed blocks until `finish()`, as wcb.h does. Streaming and flushing on byte or latency budgets is a WCB3 item.
- 234 streams from the same synthetic designs; not independent real IPs.

## Reproduce

```
g++ -O3 -std=c++17 bench.cpp -lzstd -o bench
F=$(find data -type f -name '*.wcb' ! -name '._*')
./bench 9 16384 1 3 $F; echo $?            # Preserve (default)
POLICY=skip ./bench 3 16384 1 3 $F         # opt-in policy
```

## Old vs new fast encoder, interleaved in one process (Linux host, load ~3.8, best of 3, init included, µs/change)

| | zstd-9 all | zstd-9 wide | zstd-3 all | zstd-3 wide | bytes |
|---|---:|---:|---:|---:|---|
| wcb.h today | 0.890 | 1.302 | 0.621 | 0.915 | baseline |
| old fast, always-DICT | 0.590 | 0.820 | 0.290 | 0.381 | identical |
| **new fast, Preserve (default)** | **0.448** | **0.602** | **0.236** | **0.304** | identical |
| old fast, skip | 0.384 | 0.490 | 0.218 | 0.267 | +0.044% / +0.208% |
| new fast, SkipNoRepeat | 0.326 | 0.407 | 0.190 | 0.230 | +0.044% / +0.208% |

The new version is ~1.2–1.3× faster than the old one at equal output. Most likely cause (not profiled): the old one allocated and zeroed MiBs of scratch per encoder in init(). Against wcb.h it is 2.0× (zstd-9) and 2.6× (zstd-3) in this run, vs 2.6× / 3.8× in the bench.cpp run. The ratio moves with host load and run order, so quote a 2–4× range, not a single number. `wcbfast_old.h` is rebuilt from the diff for this comparison only.

## LOD bucket shift (2026-11-07 ~03:55 CST)

`change()` computed one 64-bit division per LOD level. The buckets are lodBase·4^L, so floor(t/(base·4^L)) = floor(t/base) >> 2L. That is now one division per change, plus shifts.

Byte-identical on 234/234 streams at zstd-3. Linux host, load ~0.6, 2 interleaved runs, best of 3, init included, µs/change:

| Signals | Before | After | Change |
|---|---:|---:|---:|
| narrow (≤64 bits, 1.59 M changes) | 0.130 | 0.087 | −33% |
| wide (>64 bits) | 0.312 | 0.270 | −13% |
| all | 0.243 | 0.200 | −18% |

The narrow figures are derived: all × n_all − wide × n_wide.

## DICT hash-table clustering fix (2026-11-07 ~04:00 CST; found in code review)

The DICT table was indexed by `hmix(...) & (cap-1)`. hmix's low bits are weak for counter-like values: 64- and 512-bit counters averaged more than 2,300 occupied-slot probes per lookup (a reviewer's bucket test), and callgrind put 75% of a counter run in that probe loop. Now `slotOf()` applies murmur3 fmix64 before masking. Only the probe position changes; DICT indices follow first-occurrence order, so the bytes are unchanged. Linux host, µs/change, zstd-3:

| Workload | Before | After |
|---|---:|---:|
| 100 mixed-width counters (1–64 bits), full 16K blocks, round-robin | 0.377 | **0.075** (5.0×) |
| 2,000 counters, 2K-change blocks | 0.099 | 0.107 (+8%, within run noise or fmix cost; not separated) |
| 234-stream corpus | 0.200 | 0.202 (byte-identical 234/234 at z9 and z3) |

A separate prototype of the same fix, on matched VCS runs: mixed-counter CPU went from 2.33 s to 0.88 s, against native FSDB's 1.09 s, with WCB 73.5% smaller.
