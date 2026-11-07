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
static inline uint64_t getVar(const uint8_t *&p) { uint64_t v = *p++; if (v < 0x80) return v; v &= 0x7f; int s = 7; for (;;) { uint8_t c = *p++; v |= (uint64_t)(c & 0x7f) << s; if (!(c & 0x80)) return v; s += 7; } }
static inline uint64_t hmix(uint64_t h, uint64_t t, const uint8_t *v, size_t n) {
  uint64_t a = 0x9E3779B97F4A7C15ull ^ n; size_t i = 0;
  for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, v + i, 8); a ^= w; a *= 0xff51afd7ed558ccdull; a ^= a >> 32; }
  for (; i < n; i++) { a ^= v[i]; a *= 0x100000001b3ull; }
  h ^= a ^ (t * 0xC2B2AE3D27D4EB4Full); h *= 0x100000001b3ull; h ^= h >> 29; return h;
}

// Streaming encoder.
struct Encoder {
  int bits = 0, vlen = 0; bool fs = true; int level = 3; uint32_t blkN = 16384; uint64_t lodBase = 0; int nlev = 0; uint32_t lodN = 16384;
  Hdr h{}; std::vector<BlkEnt> blks; std::vector<uint8_t> data;
  std::vector<uint64_t> bt; std::vector<uint8_t> bv; bool bx = false;  // current block
  struct Lev { std::vector<uint8_t> buf; uint64_t cur = ~0ull, cnt = 0, last = 0, chunkB0 = 0, entries = 0; uint32_t inChunk = 0; uint8_t fl = 0; std::vector<LodEnt> ch; std::vector<uint8_t> d; };
  std::vector<Lev> lev; ZSTD_CCtx *zc = nullptr; std::vector<uint8_t> cb;
  uint64_t modeCount[2] = {0, 0}, twoState = 0, rawBytes = 0;
  void init(int b, int vl, bool fourState, uint64_t tmax, uint64_t base, int zl, uint32_t bn) {
    bits = b; vlen = vl; fs = fourState; level = zl; blkN = bn; lodBase = base;
    h = Hdr{}; h.magic = MAGIC; h.bits = b; h.vlen = vl; h.fourState = fs; h.lodBase = base; h.hash = 1469598103934665603ull;
    nlev = 0; if (base) while ((base << (2 * nlev)) < tmax / 1024 && nlev < 24) nlev++;
    nlev++;
    if (!base) nlev = 0;
    lev.assign(nlev, Lev()); blks.clear(); data.clear(); bt.clear(); bv.clear(); bx = false;
    if (!zc) zc = ZSTD_createCCtx();
  }
  size_t z(const std::vector<uint8_t> &raw) { cb.resize(ZSTD_compressBound(raw.size())); size_t r = ZSTD_compressCCtx(zc, cb.data(), cb.size(), raw.data(), raw.size(), level); cb.resize(r); return r; }
  void encodeBlock() {
    uint32_t n = bt.size(); if (!n) return;
    int w = (fs && !bx) ? vlen / 2 : vlen; uint8_t flags = (fs && !bx) ? F_2ST : 0;
    std::vector<uint8_t> hd; uint64_t tp = bt[0]; for (uint32_t i = 0; i < n; i++) { putVar(hd, bt[i] - tp); tp = bt[i]; }
    std::vector<uint8_t> x = hd, d = hd; std::vector<uint8_t> prev(w, 0);
    // dictionary: open addressing on value hash
    uint32_t cap = 1; while (cap < 2 * n) cap <<= 1; std::vector<int32_t> tab(cap, -1); std::vector<uint64_t> tagh(cap, 0); std::vector<uint32_t> dvi; uint32_t nd = 0;
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t *v = &bv[(size_t)i * vlen];
      for (int j = 0; j < w; j++) x.push_back(v[j] ^ prev[j]);
      uint64_t hh = hmix(0, 0, v, w); uint32_t s = hh & (cap - 1); int found = -1;
      while (tab[s] >= 0) { if (tagh[s] == hh && !memcmp(&bv[(size_t)dvi[tab[s]] * vlen], v, w)) { found = tab[s]; break; } s = (s + 1) & (cap - 1); }
      if (found >= 0) putVar(d, found + 1);
      else { d.push_back(0); for (int j = 0; j < w; j++) d.push_back(v[j] ^ prev[j]); tab[s] = nd++; tagh[s] = hh; dvi.push_back(i); }
      memcpy(prev.data(), v, w);
    }
    size_t cx = z(x); std::vector<uint8_t> zx = cb; size_t cd = z(d);
    BlkEnt e{}; e.t0 = bt[0]; e.off = data.size(); e.n = n; e.flags = flags;
    if (cd < cx) { e.mode = M_DICT; e.csize = cd; e.rsize = d.size(); data.insert(data.end(), cb.begin(), cb.end()); }
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
    size_t c = z(L.buf); LodEnt e{}; e.b0 = L.chunkB0; e.off = L.d.size(); e.csize = c; e.n = L.inChunk; e.rsize = L.buf.size();
    L.d.insert(L.d.end(), cb.begin(), cb.end()); L.ch.push_back(e); L.buf.clear(); L.inChunk = 0;
  }
  void change(uint64_t t, const uint8_t *v) {
    if (h.n == 0) h.t0 = t;
    h.tlast = t; h.n++; h.hash = hmix(h.hash, t, v, vlen);
    bool anyx = false; if (fs) for (int j = vlen / 2; j < vlen; j++) if (v[j]) { anyx = true; break; }
    bx |= anyx; bt.push_back(t); bv.insert(bv.end(), v, v + vlen);
    for (int li = 0; li < nlev; li++) {
      Lev &L = lev[li]; uint64_t b = t / (lodBase << (2 * li));
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
};

// Read side over a mapped file image.
struct View {
  const uint8_t *m = nullptr; size_t size = 0; const Hdr *h = nullptr; const BlkEnt *blk = nullptr; const LevEnt *lev = nullptr; const LodEnt *lod = nullptr;
  bool open(const uint8_t *p, size_t n) {
    m = p; size = n; h = (const Hdr *)p; if (h->magic != MAGIC && h->magic != MAGIC1) return false;
    blk = (const BlkEnt *)(p + sizeof(Hdr)); lev = (const LevEnt *)(blk + h->nblk); lod = (const LodEnt *)(lev + h->nlev); return true;
  }
  // first block that can hold the value in effect at t
  uint32_t blockAt(uint64_t t) const { uint32_t lo = 0, hi = h->nblk; while (lo < hi) { uint32_t md = (lo + hi) / 2; if (blk[md].t0 <= t) lo = md + 1; else hi = md; } return lo ? lo - 1 : 0; }
};
// Decoded block: times + full-width (vlen) values.
struct Decoded { std::vector<uint64_t> t; std::vector<uint8_t> v; uint32_t n = 0; };
static void decodeBlock(ZSTD_DCtx *zd, const View &V, uint32_t bi, Decoded &out, std::vector<uint8_t> &tmp) {
  const BlkEnt &e = V.blk[bi]; tmp.resize(e.rsize);
  ZSTD_decompressDCtx(zd, tmp.data(), e.rsize, V.m + e.off, e.csize);
  const uint8_t *p = tmp.data(); uint32_t n = e.n; int vl = V.h->vlen, w = (e.flags & F_2ST) ? vl / 2 : vl;
  out.n = n; out.t.resize(n); out.v.assign((size_t)n * vl, 0);
  uint64_t t = e.t0; for (uint32_t i = 0; i < n; i++) { t += getVar(p); out.t[i] = t; }
  uint8_t *v = out.v.data(); static thread_local std::vector<uint32_t> dict;
  if (e.mode == M_XOR) {
    for (uint32_t i = 0; i < n; i++) { uint8_t *c = v + (size_t)i * vl; const uint8_t *pr = i ? c - vl : nullptr;
      if (pr) for (int j = 0; j < w; j++) c[j] = pr[j] ^ p[j]; else memcpy(c, p, w); p += w; }
  } else {
    dict.clear();
    for (uint32_t i = 0; i < n; i++) { uint8_t *c = v + (size_t)i * vl; uint64_t k = getVar(p);
      if (k == 0) { const uint8_t *pr = i ? c - vl : nullptr; if (pr) for (int j = 0; j < w; j++) c[j] = pr[j] ^ p[j]; else memcpy(c, p, w); p += w; dict.push_back(i); }
      else memcpy(c, v + (size_t)dict[k - 1] * vl, w); }
  }
}
struct LodItem { uint64_t b; uint32_t cnt; uint8_t fl; };
static void decodeLod(ZSTD_DCtx *zd, const View &V, uint32_t ci, std::vector<LodItem> &out, std::vector<uint8_t> &tmp) {
  const LodEnt &e = V.lod[ci]; tmp.resize(e.rsize); ZSTD_decompressDCtx(zd, tmp.data(), e.rsize, V.m + e.off, e.csize);
  const uint8_t *p = tmp.data(); out.resize(e.n); uint64_t b = e.b0;
  if (V.h->magic == MAGIC1) { for (uint32_t i = 0; i < e.n; i++) { b += getVar(p); out[i].b = b; out[i].cnt = (uint32_t)getVar(p); out[i].fl = *p++; } return; }
  for (uint32_t i = 0; i < e.n; i++) { b += getVar(p); out[i].b = b; uint8_t c = *p++; out[i].cnt = c & 3; out[i].fl = c >> 2; }
}
}  // namespace wcb
