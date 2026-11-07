# WCB vs FSDB: where it stands (2026-11-07)

A summary of every head-to-head measurement so far. Hosts vary by section and are named in each one. Ratios move with host load, so ranges are quoted rather than single numbers. Cells that are not measured, or could not be reproduced for this summary, are left out rather than estimated. Anything marked *estimate* was not measured.

## 1. End-to-end capture inside VCS

Matched runs on one VCS host: the same design, signals and window, with native FSDB dumping compared against direct WCB output. These are the most reliable numbers here.

| Workload | FSDB wall / CPU (s) | WCB zstd-3 wall / CPU (s) | Speed | Size | Peak RSS FSDB → WCB |
|---|---:|---:|---|---:|---|
| 8 × 512-bit sparse vectors | 2.97 / 3.32 | 1.92 / 1.90 | 1.54× faster | −37.6% | 589 → 158 MiB |
| 8 × 512-bit shifting vectors | 3.21 / 3.49 | 1.83 / 1.80 | 1.76× faster | −24.9% | 604 → 155 MiB |
| Mixed counters, before the hash fix | 0.91 / 1.09 | 2.36 / 2.33 | 2.6× slower | −73.5% | 320 → 131 MiB |
| Mixed counters, after the hash fix | 0.91 / 1.09 | 0.89 / 0.88 | wall ties; 19% less CPU | −73.5% | — |
| 16-core SoC trace | 0.88 / 0.83 | — | ties at zstd-3 | −21.5% | 145 → 138 MiB |

At zstd-9 the files get smaller, but capture is slower than FSDB on the wide workloads: 4.5 s and 3.8 s, against FSDB's 3.0 s and 3.2 s. The counters stay close (0.98 s, with the hash fix applied).

### 1.1 Real design: CVA6 (2026-11-07)

CVA6 running CoreMark, first 500K cycles, 47 signals. The same `-debug_access+r` executable is used for every mode, on an exclusive host, with 3 shuffled reps; medians are given. Overhead is measured against the same run with no dump (52.09 s wall / 51.58 s CPU).

| Mode | Wall overhead | CPU overhead | File size | Writer peak RSS |
|---|---:|---:|---:|---:|
| FSDB | +1.12 s (+2.2%) | +1.46 s (+2.8%) | 1,749,497 B | 254 MiB |
| WCB, VPI value-change callbacks | +4.35 s (+8.4%) | +4.31 s (+8.4%) | 354,121 B | — |
| WCB, DPI group tap | +1.89 s (+3.6%) | +1.87 s (+3.6%) | 347,873 B | — |
| WCB, DPI group tap + background encode thread | **+0.86 s (+1.7%)** | **+1.23 s (+2.4%)** | **347,873 B** | **199 MiB** |

- The best WCB mode costs slightly less than FSDB, and its file is 5.0× smaller. The rep ranges don't overlap FSDB's, but the margin is small: call it "at or slightly under FSDB" on this run.
- The same capture modes passed strict, event-by-event comparisons against FSDB at 200K cycles (1,319,132 events). A glitch fixture (X/Z, force/release, #0 pulses, time-0 and final-tick changes) matched FSDB's glitch mode exactly (181,244 events). A kill -9 of the writing simulation recovered an exact prefix in 200/200 trials.
- Not yet shown: validation of these exact 500K outputs, and scaling from 47 signals to a whole core's signal set.

## 2. Encoder CPU alone

Offline, Linux x86-64 host, 234 real WCB2 streams, µs/change. Full tables are in [`measurements.md`](measurements.md).

| Encoder | zstd-9 | zstd-3 |
|---|---:|---:|
| FSDB native writer (reference) | 0.80–0.85 | — |
| `wcb.h` (original encoder) | 0.89–1.22 | 0.62–0.93 |
| `wcbfast`, `Preserve` (default, byte-identical) | 0.45–0.46 | 0.20–0.25 |
| `wcbfast`, `SkipNoRepeat` (opt-in, +0.04–0.2% bytes) | 0.33–0.34 | 0.19–0.20 |

- `wcbfast` is 2–4× faster than `wcb.h` at identical output.
- Later tweaks: the LOD bucket shift cut another 18% overall (33% on narrow signals), and the DICT hash-slot fix made counters 5× faster (0.377 → 0.075).
- Caveat: these numbers leave out VCS capture. In the matched runs, capture plus conversion alone took 0.7–1.2 s before any encoding.

## 3. Size

| Comparison | Result |
|---|---|
| Matched VCS runs (§1) | WCB 21.5–73.5% smaller on every workload |
| 512-bit structs, bytes per change (synthetic) | FSDB 1.344; WCB2 0.944; changed-field coding (study, spec §5.1) 0.539 |
| Nine open-source designs, total (per-design table in the paper draft) | FSDB 2,267 MB; FSDB `-compact` 890 MB; FST 770.5 MB; WCZ 634.6 MB. WCZ is smaller than `-compact` on 9/9 (1.40× in total) and than FST on 8/9 (1.21× in total, 0.89–5.27× per design). FST settings are being re-checked, and FST at its strongest setting is being added |
| CVA6 capture, 47 signals, 500K cycles (§1.1) | FSDB 1.75 MB; WCB 0.35 MB (5.0× smaller) |
| Whole stored set of a 1.72 GB FSDB | 804 MB of WCB2 (2.1× smaller); *estimate* ~490–580 MB with the study codecs |

## 4. Reads

| Test | Result |
|---|---|
| Full readback, 4 M events (matched runs) | WCB 19–21× faster |
| Full scan of FSDB's stored signals only (38 signals, 4,216,962 changes, CVA6 1M-cycle capture, exclusive host, 3 × 10 scans, identical hash on every scan) | best FSDB configuration 0.318 s wall / 0.377 s CPU; WCB 0.033 / 0.032 s; **9.64× wall, 11.78× CPU** |
| Decode CPU per value | FSDB 3.56 µs; WCB 0.082 µs (0.022 optimised); ~43× faster. The hardened reader (2026-11-07) costs 6–10% more per value (corpus 0.0224 → 0.0238 µs), still about 150× below FSDB (different sample set) |
| Random point query, FSDB loading a window per query (matched runs) | FSDB 23–56 ms; WCB 1.1–3.5 ms; 7–50× faster |
| Point query, all data resident in memory (matched runs) | WCB 6–8× faster |
| One-time load before resident queries | FSDB 0.09–0.13 s; WCB 0.09–0.21 s; WCB is slower on the wide sets |
| Stored point read, 20 signals, against FSDB's best warm reader (Linux host, real dump) | FSDB 1.94 ms; WCB 2.17 ms today, 0.64 ms with prefix decode |

The last row matters most for a viewer. Against a warm reader that is already loaded, WCB loses on point reads today. Prefix decode (decode only up to the queried time) plus a decoded-block cache is therefore required in a viewer backend. It is not in `wcb.h` yet.

## 5. Checkpoints (real VCS)

| | Overhead |
|---|---|
| Blocking `$save` every 25K cycles | +103% wall (+25% in a second run configuration) |
| Fork-save at the same interval | +0.2–0.4% wall; the parent pauses ~1.9 ms per save; no extra licence seat |
| Storage | 56 MB base plus 0.07–0.11 MB per checkpoint, stored as a delta |
| Restore from base plus delta | 0.9–1.2 s, byte-identical |

## 6. Viewer scheduling (FSDB-backed viewer, for context)

These changes are in the viewer's FSDB fetch path, not in WCB. They set the baseline that a WCB backend has to beat.

| Change | Before → after |
|---|---|
| Visible reads go ahead of prefetch | 2,841 → 695 ms; 1,051 → 432 ms; 740 → 281 ms |
| A search that finds nothing, on a huge dump | 22 s → 3–4 s |
| Reader budget 3 → 2 | may be 4–12% slower; an alternating A/B run is in progress |
| Clock descriptor (*estimate*) | edge queries ~650 ms → microseconds; later pans 0.86 → ~0.6 s |

## Bottom line

**WCB wins clearly on:**
- size, in every test;
- full readback, about 20×;
- decode CPU, about 40×;
- resident point queries, 6–8×;
- capture overhead on a real core (CVA6, 47 signals): at or slightly under FSDB, with a 5× smaller file (§1.1);
- capture of wide vectors, 1.5–1.8×;
- capture memory, about 4× less on the wide vectors;
- checkpoints, by orders of magnitude (fork-save against blocking `$save`).

**It ties on:** counter and SoC capture at zstd-3, now that the hash fix is in.

**It loses today on:**
- zstd-9 capture of wide signals;
- stored point reads against a warm FSDB reader, until prefix decode lands;
- the one-time load of wide data.

**Not measured yet:**
- capture on real IPs (CVA6 is pending);
- long runs with bounded memory, because the encoder still holds all compressed data in memory until close (spec §9 item 6);
- event order across signals (spec §9).
