// wcbfast.h: a faster WCB2 encoder. Same file format as wcb.h, so readers are unchanged.
// API: init / change / finish / blkBytes / lodBytes, as wcb::Encoder (tested usage). Copyable and movable.
//
// Changes vs wcb::Encoder:
//   - raw-pointer writes into contiguous buffers (no per-byte push_back); word-wise (8-byte) XOR and x/z checks
//   - large scratch (both raw candidates, both compressed buffers, the DICT hash table) and the ZSTD_CCtx live in
//     ONE per-thread Workspace shared by every encoder on that thread, not one per signal
//   - per-signal state is only the current block's events (grown on demand, like wcb.h) plus the compressed history
//   - DICT policy:
//       Policy::Preserve (DEFAULT): always build and compress both candidates, keep the smaller. Byte-identical to
//         wcb::Encoder (measured 215/215 streams).
//       Policy::SkipNoRepeat (opt-in): skip DICT when no value repeats in the block. Faster, but NOT size-preserving:
//         on 512-bit structs a DICT payload with no repeats (an extra 0 byte per row) often compresses smaller than
//         XOR. Measured +0.05% (zstd-9) / +0.22% (zstd-3).
// Still retains all compressed blocks until finish(), as wcb.h does; streaming is a WCB3 item.
#pragma once
#include "wcb.h"
#include <algorithm>

namespace wcbfast {
using wcb::Hdr; using wcb::BlkEnt; using wcb::LevEnt; using wcb::LodEnt;
enum class Policy { Preserve, SkipNoRepeat };

static inline uint8_t *putVarP(uint8_t *p, uint64_t v) { while (v >= 0x80) { *p++ = (uint8_t)(v | 0x80); v >>= 7; } *p++ = (uint8_t)v; return p; }
static inline void xorRow(uint8_t *dst, const uint8_t *a, const uint8_t *b, int w) {
  int j = 0;
  for (; j + 8 <= w; j += 8) { uint64_t x, y; memcpy(&x, a + j, 8); memcpy(&y, b + j, 8); x ^= y; memcpy(dst + j, &x, 8); }
  for (; j < w; j++) dst[j] = a[j] ^ b[j];
}
// Table index for the DICT hash table. hmix's low bits are weak for counter-like values (long linear-probe
// clusters: >2,000 probes per lookup on 64/512-bit counters), so finalize before masking (murmur3 fmix64).
// Only the probe position changes: DICT indices follow first-occurrence order, so output bytes are unchanged.
static inline uint32_t slotOf(uint64_t h, uint32_t cap) { h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33; return (uint32_t)h & (cap - 1); }
static inline bool anyNonzero(const uint8_t *p, int n) {
  int j = 0; uint64_t acc = 0;
  for (; j + 8 <= n; j += 8) { uint64_t x; memcpy(&x, p + j, 8); acc |= x; }
  for (; j < n; j++) acc |= p[j];
  return acc != 0;
}

// Per-thread scratch, sized to the largest block seen on this thread and reused by every encoder.
struct Workspace {
  std::vector<uint8_t> xb, db, cbx, cbd, zero; std::vector<int32_t> tab; std::vector<uint64_t> tagh; std::vector<uint32_t> dvi;
  ZSTD_CCtx *zc = nullptr; int zl = -1000;
  ~Workspace() { if (zc) ZSTD_freeCCtx(zc); }
  void ensure(size_t n, int vlen) {
    size_t maxRaw = n * 10 + n * ((size_t)vlen + 10);
    if (xb.size() < maxRaw) { xb.resize(maxRaw); db.resize(maxRaw); cbx.resize(ZSTD_compressBound(maxRaw)); cbd.resize(ZSTD_compressBound(maxRaw)); }
    if (zero.size() < (size_t)vlen) zero.assign(vlen, 0);
    uint32_t cap = 1; while (cap < 2 * n) cap <<= 1; if (tab.size() < cap) { tab.assign(cap, -1); tagh.assign(cap, 0); }
    if (!zc) zc = ZSTD_createCCtx();
  }
  size_t z(int level, const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    if (level != zl) { ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, level); zl = level; }
    return ZSTD_compress2(zc, dst, cap, src, n);
  }
  size_t bytes() const { return xb.capacity() + db.capacity() + cbx.capacity() + cbd.capacity() + zero.capacity() + tab.capacity() * 4 + tagh.capacity() * 8 + dvi.capacity() * 4; }
};
static inline Workspace &ws() { static thread_local Workspace w; return w; }

struct Encoder {
  int bits = 0, vlen = 0; bool fs = true; int level = 3; uint32_t blkN = 16384; uint64_t lodBase = 0; int nlev = 0; uint32_t lodN = 16384;
  Policy policy = Policy::Preserve;
  Hdr h{}; std::vector<BlkEnt> blks; std::vector<uint8_t> data;
  std::vector<uint64_t> bt; std::vector<uint8_t> bv; bool bx = false;  // current block, grown on demand
  struct Lev { std::vector<uint8_t> buf; uint64_t cur = ~0ull, cnt = 0, last = 0, chunkB0 = 0, entries = 0; uint32_t inChunk = 0; uint8_t fl = 0; std::vector<LodEnt> ch; std::vector<uint8_t> d; };
  std::vector<Lev> lev;
  uint64_t modeCount[2] = {0, 0}, dictTried = 0, twoState = 0, rawBytes = 0;

  void init(int b, int vl, bool fourState, uint64_t tmax, uint64_t base, int zl, uint32_t bn) {
    bits = b; vlen = vl; fs = fourState; level = zl; blkN = bn; lodBase = base;
    h = Hdr{}; h.magic = wcb::MAGIC; h.bits = b; h.vlen = vl; h.fourState = fs; h.lodBase = base; h.hash = 1469598103934665603ull;
    nlev = 0; if (base) while ((base << (2 * nlev)) < tmax / 1024 && nlev < 24) nlev++;
    nlev++; if (!base) nlev = 0;
    lev.assign(nlev, Lev()); blks.clear(); data.clear(); bt.clear(); bv.clear(); bx = false;
  }

  void encodeBlock() {
    const uint32_t n = bt.size(); if (!n) return;
    Workspace &W = ws(); W.ensure(n, vlen);
    const int w = (fs && !bx) ? vlen / 2 : vlen; const uint8_t flags = (fs && !bx) ? wcb::F_2ST : 0;
    uint8_t *p = W.xb.data(); uint64_t tp = bt[0];
    for (uint32_t i = 0; i < n; i++) { p = putVarP(p, bt[i] - tp); tp = bt[i]; }
    const size_t hdLen = p - W.xb.data();
    const uint8_t *prev = W.zero.data();
    for (uint32_t i = 0; i < n; i++) { const uint8_t *v = &bv[(size_t)i * vlen]; xorRow(p, v, prev, w); p += w; prev = v; }
    const size_t xLen = p - W.xb.data();
    size_t cx = W.z(level, W.xb.data(), xLen, W.cbx.data(), W.cbx.size());
    bool tryDict = policy == Policy::Preserve;
    uint32_t cap = 1; while (cap < 2 * n) cap <<= 1;  // same table size as wcb.h (probe order is irrelevant to output)
    if (!tryDict) {  // SkipNoRepeat: exact repeat scan first
      std::fill(W.tab.begin(), W.tab.begin() + cap, -1); W.dvi.clear(); uint32_t nd = 0;
      for (uint32_t i = 0; i < n && !tryDict; i++) {
        const uint8_t *v = &bv[(size_t)i * vlen]; uint64_t hh = wcb::hmix(0, 0, v, w); uint32_t s = slotOf(hh, cap);
        while (W.tab[s] >= 0) { if (W.tagh[s] == hh && !memcmp(&bv[(size_t)W.dvi[W.tab[s]] * vlen], v, w)) { tryDict = true; break; } s = (s + 1) & (cap - 1); }
        if (!tryDict) { W.tab[s] = nd++; W.tagh[s] = hh; W.dvi.push_back(i); }
      }
    }
    BlkEnt e{}; e.t0 = bt[0]; e.off = data.size(); e.n = n; e.flags = flags;
    bool useDict = false; size_t cd = 0, dLen = 0;
    if (tryDict) {
      dictTried++;
      memcpy(W.db.data(), W.xb.data(), hdLen); uint8_t *q = W.db.data() + hdLen;
      std::fill(W.tab.begin(), W.tab.begin() + cap, -1); W.dvi.clear(); uint32_t nd = 0; prev = W.zero.data();
      for (uint32_t i = 0; i < n; i++) {
        const uint8_t *v = &bv[(size_t)i * vlen]; uint64_t hh = wcb::hmix(0, 0, v, w); uint32_t s = slotOf(hh, cap); int found = -1;
        while (W.tab[s] >= 0) { if (W.tagh[s] == hh && !memcmp(&bv[(size_t)W.dvi[W.tab[s]] * vlen], v, w)) { found = W.tab[s]; break; } s = (s + 1) & (cap - 1); }
        if (found >= 0) q = putVarP(q, (uint64_t)found + 1);
        else { *q++ = 0; xorRow(q, v, prev, w); q += w; W.tab[s] = nd++; W.tagh[s] = hh; W.dvi.push_back(i); }
        prev = v;
      }
      dLen = q - W.db.data(); cd = W.z(level, W.db.data(), dLen, W.cbd.data(), W.cbd.size()); useDict = cd < cx;
    }
    if (useDict) { e.mode = wcb::M_DICT; e.csize = cd; e.rsize = dLen; data.insert(data.end(), W.cbd.begin(), W.cbd.begin() + cd); }
    else { e.mode = wcb::M_XOR; e.csize = cx; e.rsize = xLen; data.insert(data.end(), W.cbx.begin(), W.cbx.begin() + cx); }
    modeCount[e.mode]++; twoState += flags & wcb::F_2ST; rawBytes += e.rsize;
    blks.push_back(e); bt.clear(); bv.clear(); bx = false;
  }
  void lodFlushEntry(Lev &L) {
    if (L.cur == ~0ull) return;
    if (L.inChunk == 0) L.chunkB0 = L.cur;
    wcb::putVar(L.buf, L.cur - (L.inChunk ? L.last : L.chunkB0)); L.buf.push_back((uint8_t)((L.cnt < 2 ? L.cnt : 2) | (L.fl << 2)));
    L.last = L.cur; L.entries++;
    if (++L.inChunk >= lodN) lodFlushChunk(L);
  }
  void lodFlushChunk(Lev &L) {
    if (!L.inChunk) return;
    Workspace &W = ws(); W.ensure(1, 1); size_t need = ZSTD_compressBound(L.buf.size()); if (W.cbd.size() < need) W.cbd.resize(need);
    size_t c = W.z(level, L.buf.data(), L.buf.size(), W.cbd.data(), W.cbd.size());
    LodEnt e{}; e.b0 = L.chunkB0; e.off = L.d.size(); e.csize = c; e.n = L.inChunk; e.rsize = L.buf.size();
    L.d.insert(L.d.end(), W.cbd.begin(), W.cbd.begin() + c); L.ch.push_back(e); L.buf.clear(); L.inChunk = 0;
  }
  void change(uint64_t t, const uint8_t *v) {
    if (h.n == 0) h.t0 = t;
    h.tlast = t; h.n++; h.hash = wcb::hmix(h.hash, t, v, vlen);
    bool anyx = fs && anyNonzero(v + vlen / 2, vlen / 2);
    bx |= anyx; bt.push_back(t); bv.insert(bv.end(), v, v + vlen);
    // floor(t / (base * 4^L)) == floor(t / base) >> 2L, so one division per change instead of one per level
    const uint64_t b0 = nlev ? t / lodBase : 0;
    for (int li = 0; li < nlev; li++) {
      Lev &L = lev[li]; uint64_t b = b0 >> (2 * li);
      if (b != L.cur) { lodFlushEntry(L); L.cur = b; L.cnt = 0; L.fl = 0; }
      L.cnt++; if (anyx) L.fl |= 1; if (bits == 1) L.fl = (L.fl & 1) | ((v[0] & 1) << 1);
    }
    if (bt.size() >= blkN) encodeBlock();
  }
  std::vector<uint8_t> finish() {
    encodeBlock();
    for (int li = 0; li < nlev; li++) { lodFlushEntry(lev[li]); lodFlushChunk(lev[li]); }
    h.nblk = blks.size(); h.nlev = nlev; uint32_t nch = 0; for (auto &L : lev) nch += L.ch.size(); h.nchunk = nch;
    size_t hdrSz = sizeof(Hdr) + blks.size() * sizeof(BlkEnt) + nlev * sizeof(LevEnt) + nch * sizeof(LodEnt);
    std::vector<uint8_t> f(hdrSz); uint64_t dataOff = hdrSz, lodOff = hdrSz + data.size();
    for (auto &b : blks) b.off += dataOff;  // as wcb.h: offsets become absolute after finish()
    memcpy(f.data(), &h, sizeof h); size_t p = sizeof h;
    memcpy(f.data() + p, blks.data(), blks.size() * sizeof(BlkEnt)); p += blks.size() * sizeof(BlkEnt);
    uint32_t fc = 0; uint64_t lo = lodOff;
    for (auto &L : lev) { LevEnt le{fc, (uint32_t)L.ch.size(), L.entries}; memcpy(f.data() + p, &le, sizeof le); p += sizeof le; fc += L.ch.size(); }
    for (auto &L : lev) { for (auto c : L.ch) { c.off += lo; memcpy(f.data() + p, &c, sizeof c); p += sizeof c; } lo += L.d.size(); }
    f.insert(f.end(), data.begin(), data.end());
    for (auto &L : lev) f.insert(f.end(), L.d.begin(), L.d.end());
    return f;
  }
  size_t lodBytes() const { size_t s = 0; for (auto &L : lev) s += L.d.size() + L.ch.size() * sizeof(LodEnt) + sizeof(LevEnt); return s; }
  size_t blkBytes() const { return data.size() + blks.size() * sizeof(BlkEnt) + sizeof(Hdr); }
  size_t stateBytes() const {  // per-signal memory held by this encoder (excludes the shared Workspace)
    size_t s = bt.capacity() * 8 + bv.capacity() + data.capacity() + blks.capacity() * sizeof(BlkEnt);
    for (auto &L : lev) s += L.buf.capacity() + L.d.capacity() + L.ch.capacity() * sizeof(LodEnt);
    return s;
  }
};
}  // namespace wcbfast
