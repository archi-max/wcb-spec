// wcb.h: a block-indexed, per-signal adaptive lossless waveform store (study format).
// One file per signal:
//   [Hdr][BlkEnt x nblk][LevEnt x nlev][LodEnt x sum(nchunk)][block data][lod data]
// Value change blocks: up to BLK_N changes, zstd-compressed, each decodable alone.
//   payload: varint dt[n] (dt[0] = t - blk.t0 = 0), then values by mode:
//     M_XOR:  each value XOR previous (previous = 0 at block start), w bytes
//     M_DICT: varint k; k == 0 -> w bytes XOR previous, else repeat of the k-th distinct value of the block
//   w = vlen (4-state: value plane + xz plane), or vlen/2 when the block has no x/z (flag F_2ST).
// LOD level L (bucket width = lodBase << 2L): sparse list of buckets holding >= 1 change,
//   entry = varint bucket delta, byte: bits0-1 min(count,2) (render needs only 0/1/many), bit2 any x/z, bit3 value after (1-bit)
//   in chunks of LOD_N entries, zstd-compressed.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <zstd.h>

namespace wcb {
static const uint32_t MAGIC = 0x32424357;  // "WCB2" (WCB1: LOD entry = delta, varint count, flag byte)
static const uint32_t MAGIC1 = 0x31424357;
enum { M_XOR = 0, M_DICT = 1 };
enum { F_2ST = 1 };
struct Hdr { uint32_t magic, bits, vlen, fourState; uint64_t n, t0, tlast, lodBase, hash; uint32_t nblk, nlev, nchunk, pad; };
struct BlkEnt { uint64_t t0, off; uint32_t csize, n, rsize; uint8_t mode, flags, pad[2]; };
struct LevEnt { uint32_t firstChunk, nchunk; uint64_t entries; };
struct LodEnt { uint64_t b0, off; uint32_t csize, n, rsize, pad; };  // b0 = first bucket index in chunk

static inline void putVar(std::vector<uint8_t> &b, uint64_t v) { while (v >= 0x80) { b.push_back((uint8_t)(v | 0x80)); v >>= 7; } b.push_back((uint8_t)v); }
static inline uint64_t getVar(const uint8_t *&p) { uint64_t v = *p++; if (v < 0x80) return v; v &= 0x7f; int s = 7; for (;;) { uint8_t c = *p++; v |= (uint64_t)(c & 0x7f) << s; if (!(c & 0x80)) return v; s += 7; if (s > 63) return v; } }  // UNCHECKED: trusted, writer-produced buffers only (experimental dom.h); the reader uses getVarB
static inline uint64_t hmix(uint64_t h, uint64_t t, const uint8_t *v, size_t n) {
  uint64_t a = 0x9E3779B97F4A7C15ull ^ n; size_t i = 0;
  for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, v + i, 8); a ^= w; a *= 0xff51afd7ed558ccdull; a ^= a >> 32; }
  for (; i < n; i++) { a ^= v[i]; a *= 0x100000001b3ull; }
  h ^= a ^ (t * 0xC2B2AE3D27D4EB4Full); h *= 0x100000001b3ull; h ^= h >> 29; return h;
}

// Streaming encoder.
// Per-thread encoder scratch (both raw candidates, both compressed buffers, the DICT hash table, the ZSTD_CCtx), sized to the
// largest block seen and shared by every Encoder on the thread. Per-signal state is only the open block + compressed history.
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
// DICT table slot: finalize the value hash before masking. hmix's low bits are weak for counter-like values (measured >2,300
// probes per lookup on 64/512-bit counters with a plain `hh & (cap-1)`). Output bytes do not depend on slot order.
static inline uint32_t slotOf(uint64_t h, uint32_t cap) { h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33; return (uint32_t)h & (cap - 1); }
static inline uint8_t *putVarP(uint8_t *p, uint64_t v) { while (v >= 0x80) { *p++ = (uint8_t)(v | 0x80); v >>= 7; } *p++ = (uint8_t)v; return p; }
static inline void xorRow(uint8_t *dst, const uint8_t *a, const uint8_t *b, int w) {
  int j = 0; for (; j + 8 <= w; j += 8) { uint64_t x, y; memcpy(&x, a + j, 8); memcpy(&y, b + j, 8); x ^= y; memcpy(dst + j, &x, 8); }
  for (; j < w; j++) dst[j] = a[j] ^ b[j];
}
static inline bool anyNonzero(const uint8_t *p, int n) {
  int j = 0; uint64_t acc = 0; for (; j + 8 <= n; j += 8) { uint64_t x; memcpy(&x, p + j, 8); acc |= x; } for (; j < n; j++) acc |= p[j]; return acc != 0;
}
// DICT policy. Preserve (DEFAULT): always build and compress both candidates, keep the smaller (byte-identical to encodeBlockRef).
// SkipNoRepeat (opt-in): skip DICT when no value repeats in the block; not size-preserving (a no-repeat DICT payload can still win
// on wide structs). Measured +0.044% (zstd-9) / +0.208% (zstd-3) on 234 streams.
enum class Policy { Preserve, SkipNoRepeat };

struct Encoder {
  int bits = 0, vlen = 0; bool fs = true; int level = 3; uint32_t blkN = 16384; uint64_t lodBase = 0; int nlev = 0; uint32_t lodN = 16384;
  Hdr h{}; std::vector<BlkEnt> blks; std::vector<uint8_t> data;
  std::vector<uint64_t> bt; std::vector<uint8_t> bv; bool bx = false;  // current block, grown on demand
  struct Lev { std::vector<uint8_t> buf; uint64_t cur = ~0ull, cnt = 0, last = 0, chunkB0 = 0, entries = 0; uint32_t inChunk = 0; uint8_t fl = 0; std::vector<LodEnt> ch; std::vector<uint8_t> d; };
  std::vector<Lev> lev;
  ZSTD_CCtx *zc = nullptr;  // kept for API compatibility; always null (the context lives in the per-thread Workspace)
  Policy policy = Policy::Preserve; bool refEnc = false;  // refEnc: the original byte-by-byte builder, for tests/benches only
  uint64_t modeCount[2] = {0, 0}, dictTried = 0, twoState = 0, rawBytes = 0;
  void init(int b, int vl, bool fourState, uint64_t tmax, uint64_t base, int zl, uint32_t bn) {
    bits = b; vlen = vl; fs = fourState; level = zl; blkN = bn; lodBase = base;
    h = Hdr{}; h.magic = MAGIC; h.bits = b; h.vlen = vl; h.fourState = fs; h.lodBase = base; h.hash = 1469598103934665603ull;
    nlev = 0; if (base) while ((base << (2 * nlev)) < tmax / 1024 && nlev < 24) nlev++;
    nlev++;
    if (!base) nlev = 0;
    lev.assign(nlev, Lev()); blks.clear(); data.clear(); bt.clear(); bv.clear(); bx = false;
  }
  // compress into a fresh vector (used by the reference builder only)
  std::vector<uint8_t> zv(const std::vector<uint8_t> &raw) {
    Workspace &W = ws(); W.ensure(1, 1); size_t need = ZSTD_compressBound(raw.size()); if (W.cbd.size() < need) W.cbd.resize(need);
    size_t c = W.z(level, raw.data(), raw.size(), W.cbd.data(), W.cbd.size()); return std::vector<uint8_t>(W.cbd.begin(), W.cbd.begin() + c);
  }
  void encodeBlock() { if (refEnc) { encodeBlockRef(); return; }
    const uint32_t n = bt.size(); if (!n) return;
    Workspace &W = ws(); W.ensure(n, vlen);
    const int w = (fs && !bx) ? vlen / 2 : vlen; const uint8_t flags = (fs && !bx) ? F_2ST : 0;
    uint8_t *p = W.xb.data(); uint64_t tp = bt[0];
    for (uint32_t i = 0; i < n; i++) { p = putVarP(p, bt[i] - tp); tp = bt[i]; }
    const size_t hdLen = p - W.xb.data();
    const uint8_t *prev = W.zero.data();
    for (uint32_t i = 0; i < n; i++) { const uint8_t *v = &bv[(size_t)i * vlen]; xorRow(p, v, prev, w); p += w; prev = v; }
    const size_t xLen = p - W.xb.data();
    size_t cx = W.z(level, W.xb.data(), xLen, W.cbx.data(), W.cbx.size());
    bool tryDict = policy == Policy::Preserve;
    uint32_t cap = 1; while (cap < 2 * n) cap <<= 1;
    if (!tryDict) {  // SkipNoRepeat: exact repeat scan first
      std::fill(W.tab.begin(), W.tab.begin() + cap, -1); W.dvi.clear(); uint32_t nd = 0;
      for (uint32_t i = 0; i < n && !tryDict; i++) {
        const uint8_t *v = &bv[(size_t)i * vlen]; uint64_t hh = hmix(0, 0, v, w); uint32_t s = slotOf(hh, cap);
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
        const uint8_t *v = &bv[(size_t)i * vlen]; uint64_t hh = hmix(0, 0, v, w); uint32_t s = slotOf(hh, cap); int found = -1;
        while (W.tab[s] >= 0) { if (W.tagh[s] == hh && !memcmp(&bv[(size_t)W.dvi[W.tab[s]] * vlen], v, w)) { found = W.tab[s]; break; } s = (s + 1) & (cap - 1); }
        if (found >= 0) q = putVarP(q, (uint64_t)found + 1);
        else { *q++ = 0; xorRow(q, v, prev, w); q += w; W.tab[s] = nd++; W.tagh[s] = hh; W.dvi.push_back(i); }
        prev = v;
      }
      dLen = q - W.db.data(); cd = W.z(level, W.db.data(), dLen, W.cbd.data(), W.cbd.size()); useDict = cd < cx;
    }
    if (useDict) { e.mode = M_DICT; e.csize = cd; e.rsize = dLen; data.insert(data.end(), W.cbd.begin(), W.cbd.begin() + cd); }
    else { e.mode = M_XOR; e.csize = cx; e.rsize = xLen; data.insert(data.end(), W.cbx.begin(), W.cbx.begin() + cx); }
    modeCount[e.mode]++; twoState += flags & F_2ST; rawBytes += e.rsize;
    blks.push_back(e); bt.clear(); bv.clear(); bx = false;
  }
  void encodeBlockRef() {
    uint32_t n = bt.size(); if (!n) return;
    int w = (fs && !bx) ? vlen / 2 : vlen; uint8_t flags = (fs && !bx) ? F_2ST : 0;
    std::vector<uint8_t> hd; uint64_t tp = bt[0]; for (uint32_t i = 0; i < n; i++) { putVar(hd, bt[i] - tp); tp = bt[i]; }
    std::vector<uint8_t> x = hd, d = hd; std::vector<uint8_t> prev(w, 0);
    // dictionary: open addressing on value hash
    uint32_t cap = 1; while (cap < 2 * n) cap <<= 1; std::vector<int32_t> tab(cap, -1); std::vector<uint64_t> tagh(cap, 0); std::vector<uint32_t> dvi; uint32_t nd = 0;
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t *v = &bv[(size_t)i * vlen];
      for (int j = 0; j < w; j++) x.push_back(v[j] ^ prev[j]);
      uint64_t hh = hmix(0, 0, v, w); uint32_t s = slotOf(hh, cap); int found = -1;
      while (tab[s] >= 0) { if (tagh[s] == hh && !memcmp(&bv[(size_t)dvi[tab[s]] * vlen], v, w)) { found = tab[s]; break; } s = (s + 1) & (cap - 1); }
      if (found >= 0) putVar(d, found + 1);
      else { d.push_back(0); for (int j = 0; j < w; j++) d.push_back(v[j] ^ prev[j]); tab[s] = nd++; tagh[s] = hh; dvi.push_back(i); }
      memcpy(prev.data(), v, w);
    }
    std::vector<uint8_t> zx = zv(x), zd = zv(d); size_t cx = zx.size(), cd = zd.size();
    BlkEnt e{}; e.t0 = bt[0]; e.off = data.size(); e.n = n; e.flags = flags;
    if (cd < cx) { e.mode = M_DICT; e.csize = cd; e.rsize = d.size(); data.insert(data.end(), zd.begin(), zd.end()); }
    else { e.mode = M_XOR; e.csize = cx; e.rsize = x.size(); data.insert(data.end(), zx.begin(), zx.end()); }
    modeCount[e.mode]++; twoState += flags & F_2ST; rawBytes += e.rsize;
    blks.push_back(e); bt.clear(); bv.clear(); bx = false;
  }
  void lodFlushEntry(Lev &L, int li) {
    if (L.cur == ~0ull) return;
    if (L.inChunk == 0) L.chunkB0 = L.cur;
    putVar(L.buf, L.cur - (L.inChunk ? L.last : L.chunkB0)); L.buf.push_back((uint8_t)((L.cnt < 2 ? L.cnt : 2) | (L.fl << 2)));
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
    h.tlast = t; h.n++; h.hash = hmix(h.hash, t, v, vlen);
    bool anyx = fs && anyNonzero(v + vlen / 2, vlen / 2);
    bx |= anyx; bt.push_back(t); bv.insert(bv.end(), v, v + vlen);
    const uint64_t b0 = nlev ? t / lodBase : 0;  // level li bucket = t / (lodBase * 4^li) = b0 >> 2li (exact: floor(floor(t/B)/4^L) = floor(t/(B 4^L)))
    for (int li = 0; li < nlev; li++) {
      Lev &L = lev[li]; uint64_t b = b0 >> (2 * li);
      if (b != L.cur) { lodFlushEntry(L, li); L.cur = b; L.cnt = 0; L.fl = 0; }
      L.cnt++; if (anyx) L.fl |= 1; if (bits == 1) L.fl = (L.fl & 1) | ((v[0] & 1) << 1);
    }
    if (bt.size() >= blkN) encodeBlock();
  }
  // returns the file image
  std::vector<uint8_t> finish() {
    encodeBlock();
    for (int li = 0; li < nlev; li++) { lodFlushEntry(lev[li], li); lodFlushChunk(lev[li]); }
    h.nblk = blks.size(); h.nlev = nlev; uint32_t nch = 0; for (auto &L : lev) nch += L.ch.size(); h.nchunk = nch;
    size_t hdrSz = sizeof(Hdr) + blks.size() * sizeof(BlkEnt) + nlev * sizeof(LevEnt) + nch * sizeof(LodEnt);
    std::vector<uint8_t> f(hdrSz);
    uint64_t dataOff = hdrSz, lodOff = hdrSz + data.size();
    for (auto &b : blks) b.off += dataOff;
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

// Read side over a mapped file image.
// ---- Reader. Every function below validates its input and is safe on untrusted bytes: no read outside the mapped file or the
// decompressed buffer, bounded allocations, and failure is atomic (nothing partly decoded is exposed as valid).
// Allocation limits (per call); raise them only together with a matching writer limit.
static const uint32_t MAX_BLK_EVENTS = 1u << 24;          // changes per block (the writer default is 16,384)
static const uint64_t MAX_DECODED_BYTES = 1ull << 30;    // decoded values per block (n * vlen) and decompressed payload size
static const uint32_t MAX_VLEN = 1u << 20;               // bytes per value (4-state: 2 planes)
// Strict LEB128 varint: reads only [p, end); rejects truncation (no terminator before end), more than 10 bytes, a 10th byte that
// overflows 64 bits, and non-canonical (overlong) encodings such as 0x80 0x00, which the writer never produces.
static inline bool getVarB(const uint8_t *&p, const uint8_t *end, uint64_t &v) {
  if (p >= end) return false;
  uint8_t c = *p++; v = c & 0x7f; if (!(c & 0x80)) return true;
  for (int s = 7;; s += 7) {
    if (p >= end) return false;
    c = *p++;
    if (s == 63) { if (c > 1) return false; v |= (uint64_t)c << 63; return c != 0; }  // 10th byte: only bit 0 fits, no continuation
    v |= (uint64_t)(c & 0x7f) << s;
    if (!(c & 0x80)) return c != 0;  // a zero final byte after a continuation is an overlong encoding
  }
}
// Same rules as getVarB when at least 10 bytes remain (so no end check is needed per byte); the caller guarantees end - p >= 10.
static inline bool getVar10(const uint8_t *&p, uint64_t &v) {
  uint8_t c = *p++; v = c & 0x7f; if (!(c & 0x80)) return true;
  for (int s = 7;; s += 7) {
    c = *p++;
    if (s == 63) { if (c > 1) return false; v |= (uint64_t)c << 63; return c != 0; }
    v |= (uint64_t)(c & 0x7f) << s;
    if (!(c & 0x80)) return c != 0;
  }
}
static inline bool getVarC(const uint8_t *&p, const uint8_t *end, uint64_t &v) { return end - p >= 10 ? getVar10(p, v) : getVarB(p, end, v); }
struct View {
  const uint8_t *m = nullptr; size_t size = 0; const Hdr *h = nullptr; const BlkEnt *blk = nullptr; const LevEnt *lev = nullptr; const LodEnt *lod = nullptr;
  // Validates the header and every index entry (O(nblk + nchunk)), so lookups below can trust the index.
  bool open(const uint8_t *p, size_t n) {
    m = nullptr; size = 0; h = nullptr; blk = nullptr; lev = nullptr; lod = nullptr;
    if (!p || n < sizeof(Hdr)) return false;
    const Hdr *H = (const Hdr *)p;
    if (H->magic != MAGIC && H->magic != MAGIC1) return false;
    if (H->fourState > 1 || H->bits == 0 || H->vlen == 0 || H->vlen > MAX_VLEN) return false;
    if (H->vlen != (H->fourState ? 2u : 1u) * ((H->bits + 7) / 8)) return false;  // value layout: (x/z plane +) value plane, bytes
    if (H->nlev > 64) return false;
    size_t idx = sizeof(Hdr) + (size_t)H->nblk * sizeof(BlkEnt) + (size_t)H->nlev * sizeof(LevEnt) + (size_t)H->nchunk * sizeof(LodEnt);
    if (idx > n) return false;
    const BlkEnt *B = (const BlkEnt *)(p + sizeof(Hdr)); const LevEnt *L = (const LevEnt *)(B + H->nblk); const LodEnt *C = (const LodEnt *)(L + H->nlev);
    if ((H->n == 0) != (H->nblk == 0)) return false;
    uint64_t tot = 0;
    for (uint32_t i = 0; i < H->nblk; i++) {
      const BlkEnt &e = B[i];
      if (e.mode > M_DICT || (e.flags & ~(uint8_t)F_2ST) || ((e.flags & F_2ST) && !H->fourState)) return false;
      if (e.n == 0 || e.n > MAX_BLK_EVENTS || e.off < idx || e.off > n || e.csize > n - e.off || e.rsize < e.n) return false;
      if ((uint64_t)e.n * H->vlen > MAX_DECODED_BYTES || e.rsize > MAX_DECODED_BYTES) return false;
      if (i && e.t0 < B[i - 1].t0) return false;
      tot += e.n;
    }
    if (tot != H->n) return false;
    if (H->nblk && (B[0].t0 != H->t0 || B[H->nblk - 1].t0 > H->tlast)) return false;
    uint64_t chunks = 0;
    for (uint32_t l = 0; l < H->nlev; l++) { if (L[l].firstChunk != chunks || L[l].nchunk > H->nchunk - chunks) return false; chunks += L[l].nchunk; }
    if (chunks != H->nchunk || (H->nlev && !H->lodBase)) return false;
    const uint32_t minEnt = H->magic == MAGIC1 ? 3 : 2;  // smallest LOD entry: varint + byte (WCB1: varint + varint + byte)
    for (uint32_t c = 0; c < H->nchunk; c++) {
      const LodEnt &e = C[c];
      if (e.n == 0 || e.off < idx || e.off > n || e.csize > n - e.off || e.rsize > MAX_DECODED_BYTES || (uint64_t)e.n * minEnt > e.rsize) return false;
    }
    m = p; size = n; h = H; blk = B; lev = L; lod = C; return true;
  }
  // first block that can hold a change at time >= t (for change scans that include t): same-time changes may span a block boundary,
  // so start one block before the first block whose t0 >= t
  uint32_t blockFirst(uint64_t t) const { uint32_t lo = 0, hi = h->nblk; while (lo < hi) { uint32_t md = (lo + hi) / 2; if (blk[md].t0 < t) lo = md + 1; else hi = md; } return lo ? lo - 1 : 0; }
  // value-at-t lookups only: the last block with t0 <= t holds the last change <= t
  uint32_t blockAt(uint64_t t) const { uint32_t lo = 0, hi = h->nblk; while (lo < hi) { uint32_t md = (lo + hi) / 2; if (blk[md].t0 <= t) lo = md + 1; else hi = md; } return lo ? lo - 1 : 0; }
};
// Decoded block: times + full-width (vlen) values. Valid only when the decoder returned true; on failure n = 0 and t, v are empty.
struct Decoded { std::vector<uint64_t> t; std::vector<uint8_t> v; uint32_t n = 0; };
// zstd frame -> tmp[0, rsize). Rejects a frame whose declared content size differs from rsize, a ZSTD error and a short result.
static inline bool unz(ZSTD_DCtx *zd, const View &V, uint64_t off, uint32_t csize, uint32_t rsize, std::vector<uint8_t> &tmp) {
  if (!V.h || off > V.size || csize > V.size - off) return false;
  unsigned long long fcs = ZSTD_getFrameContentSize(V.m + off, csize);
  if (fcs == ZSTD_CONTENTSIZE_ERROR || (fcs != ZSTD_CONTENTSIZE_UNKNOWN && fcs != rsize)) return false;
  tmp.resize(rsize ? rsize : 1);
  size_t r = ZSTD_decompressDCtx(zd, tmp.data(), rsize, V.m + off, csize);
  return !ZSTD_isError(r) && r == rsize;
}
// Validating decoder: false on any inconsistency (see View::open for the index checks, getVarB for varints): payload size, mode,
// timestamps (first delta must be 0, i.e. t[0] == blk.t0; no u64 overflow; last <= Hdr.tlast), payload bounds, DICT references
// outside the values seen so far, and trailing bytes.
static bool decodeBlock(ZSTD_DCtx *zd, const View &V, uint32_t bi, Decoded &out, std::vector<uint8_t> &tmp) {
  out.n = 0;
  auto fail = [&out]() { out.n = 0; out.t.clear(); out.v.clear(); return false; };
  if (!V.h || bi >= V.h->nblk) return fail();
  const BlkEnt &e = V.blk[bi];  // index entry already checked by View::open (mode, flags, n, bounds, budgets)
  if (!unz(zd, V, e.off, e.csize, e.rsize, tmp)) return fail();
  const uint8_t *p = tmp.data(), *end = tmp.data() + e.rsize; const uint32_t n = e.n; const int vl = V.h->vlen, w = (e.flags & F_2ST) ? vl / 2 : vl;
  out.t.resize(n); out.v.assign((size_t)n * vl, 0);
  uint64_t t = e.t0, d;
  if (!getVarC(p, end, d) || d != 0) return fail();  // t[0] == blk.t0
  out.t[0] = t;
  for (uint32_t i = 1; i < n; i++) { if (!getVarC(p, end, d) || d > ~0ull - t) return fail(); t += d; out.t[i] = t; }
  if (t > V.h->tlast) return fail();
  uint8_t *v = out.v.data(); static thread_local std::vector<uint32_t> dict;
  if (e.mode == M_XOR) {
    if ((size_t)(end - p) != (size_t)n * w) return fail();
    for (uint32_t i = 0; i < n; i++) { uint8_t *c = v + (size_t)i * vl; const uint8_t *pr = i ? c - vl : nullptr;
      if (pr) for (int j = 0; j < w; j++) c[j] = pr[j] ^ p[j]; else memcpy(c, p, w); p += w; }
  } else {
    dict.clear();
    for (uint32_t i = 0; i < n; i++) { uint8_t *c = v + (size_t)i * vl; uint64_t k; if (!getVarC(p, end, k)) return fail();
      if (k == 0) { if ((size_t)(end - p) < (size_t)w) return fail(); const uint8_t *pr = i ? c - vl : nullptr; if (pr) for (int j = 0; j < w; j++) c[j] = pr[j] ^ p[j]; else memcpy(c, p, w); p += w; dict.push_back(i); }
      else { if (k > dict.size()) return fail(); memcpy(c, v + (size_t)dict[k - 1] * vl, w); } }
  }
  if (p != end) return fail();
  out.n = n; return true;
}
struct LodItem { uint64_t b; uint32_t cnt; uint8_t fl; };
// Validating LOD chunk decoder: false (out empty) on a bad chunk index, frame/size mismatch, bad varint, bucket overflow,
// count/flag out of range, a payload overrun or trailing bytes.
static bool decodeLod(ZSTD_DCtx *zd, const View &V, uint32_t ci, std::vector<LodItem> &out, std::vector<uint8_t> &tmp) {
  auto fail = [&out]() { out.clear(); return false; };
  if (!V.h || ci >= V.h->nchunk) return fail();
  const LodEnt &e = V.lod[ci];
  if (!unz(zd, V, e.off, e.csize, e.rsize, tmp)) return fail();
  const uint8_t *p = tmp.data(), *end = tmp.data() + e.rsize; out.resize(e.n); uint64_t b = e.b0, d;
  const bool v1 = V.h->magic == MAGIC1;
  for (uint32_t i = 0; i < e.n; i++) {
    if (!getVarC(p, end, d) || d > ~0ull - b || (i ? d == 0 : (!v1 && d != 0))) return fail();  // first entry is b0 itself, then strictly increasing
    b += d; out[i].b = b;
    if (v1) { uint64_t c; if (!getVarB(p, end, c) || c > 0xffffffffull || p >= end) return fail(); out[i].cnt = (uint32_t)c; out[i].fl = *p++; }
    else { if (p >= end) return fail(); uint8_t c = *p++; if ((c & 3) == 0 || (c & 3) == 3 || (c >> 4)) return fail(); out[i].cnt = c & 3; out[i].fl = c >> 2; }  // count = min(count, 2) >= 1; flags in bits 2-3
  }
  if (p != end) return fail();
  return true;
}
}  // namespace wcb
