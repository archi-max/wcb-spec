// dom.h: shared timestamps per clock domain over wcb blocks (study codec, lossless).
// A domain is a sorted edge-time list (e.g. the rising edges of one clock), stored once in chunks of EDGE_N
// edges (varint deltas, zstd). A member signal keeps wcb's 16K-change blocks and value payloads, but its time
// header becomes, per change: hit  -> varint((edge index - previous edge index) << 1)
//                                  miss -> varint(((t - previous t) << 1) | 1)   (escape: a change off the edge list)
// or, when denser, a bitmap over the edges the block spans plus an escape list (TK_BITMAP).
// Block index keeps the absolute t0 (random access unchanged) plus the first edge index.
#pragma once
#include "wcb.h"
#include <algorithm>
namespace wdom {
enum { TK_DT = 0, TK_IDX = 1, TK_BITMAP = 2 };
static const uint32_t EDGE_N = 16384;
struct Domain {
  std::vector<uint64_t> e;  // sorted edge times
  size_t idxOf(uint64_t t) const { return std::lower_bound(e.begin(), e.end(), t) - e.begin(); }  // first edge >= t
  bool has(uint64_t t, size_t &i) const { i = idxOf(t); return i < e.size() && e[i] == t; }
  // stored size: chunks of EDGE_N varint deltas, zstd-3, plus a (t0, off, csize) entry per chunk
  size_t storedBytes(ZSTD_CCtx *zc, int zl) const {
    size_t s = 0; std::vector<uint8_t> raw, cb;
    for (size_t i = 0; i < e.size(); i += EDGE_N) { raw.clear(); uint64_t p = e[i]; for (size_t j = i; j < std::min(e.size(), i + EDGE_N); j++) { wcb::putVar(raw, e[j] - p); p = e[j]; }
      cb.resize(ZSTD_compressBound(raw.size())); s += ZSTD_compressCCtx(zc, cb.data(), cb.size(), raw.data(), raw.size(), zl) + 24; }
    return s;
  }
};
// value payload exactly as wcb::Encoder builds it (without the time header); returns mode
static int valuePayload(ZSTD_CCtx *zc, int zl, const uint8_t *bv, uint32_t n, int vlen, int w, std::vector<uint8_t> &out) {
  std::vector<uint8_t> x, d, prev(w, 0), cb;
  uint32_t cap = 1; while (cap < 2 * n) cap <<= 1; std::vector<int32_t> tab(cap, -1); std::vector<uint64_t> tagh(cap, 0); std::vector<uint32_t> dvi; uint32_t nd = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *v = bv + (size_t)i * vlen;
    for (int j = 0; j < w; j++) x.push_back(v[j] ^ prev[j]);
    uint64_t hh = wcb::hmix(0, 0, v, w); uint32_t s = hh & (cap - 1); int found = -1;
    while (tab[s] >= 0) { if (tagh[s] == hh && !memcmp(bv + (size_t)dvi[tab[s]] * vlen, v, w)) { found = tab[s]; break; } s = (s + 1) & (cap - 1); }
    if (found >= 0) wcb::putVar(d, found + 1); else { d.push_back(0); for (int j = 0; j < w; j++) d.push_back(v[j] ^ prev[j]); tab[s] = nd++; tagh[s] = hh; dvi.push_back(i); }
    memcpy(prev.data(), v, w);
  }
  cb.resize(ZSTD_compressBound(std::max(x.size(), d.size())));
  size_t cx = ZSTD_compressCCtx(zc, cb.data(), cb.size(), x.data(), x.size(), zl), cd = ZSTD_compressCCtx(zc, cb.data(), cb.size(), d.data(), d.size(), zl);
  if (cd < cx) { out = d; return wcb::M_DICT; } out = x; return wcb::M_XOR;
}
static void timeDT(const uint64_t *t, uint32_t n, std::vector<uint8_t> &o) { o.clear(); uint64_t p = t[0]; for (uint32_t i = 0; i < n; i++) { wcb::putVar(o, t[i] - p); p = t[i]; } }
static uint32_t timeIDX(const Domain &D, const uint64_t *t, uint32_t n, std::vector<uint8_t> &o, uint64_t &firstEdge) {
  o.clear(); uint32_t miss = 0; size_t pi = D.idxOf(t[0]); firstEdge = pi; uint64_t pt = t[0];
  for (uint32_t i = 0; i < n; i++) { size_t k; if (D.has(t[i], k)) { wcb::putVar(o, (uint64_t)(k - pi) << 1); pi = k; } else { wcb::putVar(o, ((t[i] - pt) << 1) | 1); miss++; pi = D.idxOf(t[i]); } pt = t[i]; }
  return miss;
}
// bitmap over edges [e0, e1] (1 = a change on that edge), then varint escape count + escapes (change index delta, dt)
static void timeBMP(const Domain &D, const uint64_t *t, uint32_t n, std::vector<uint8_t> &o, uint64_t &firstEdge) {
  o.clear(); size_t e0 = D.idxOf(t[0]), e1 = D.idxOf(t[n - 1]); if (e1 >= D.e.size()) e1 = D.e.size() - 1; if (e0 > e1) e0 = e1; firstEdge = e0;
  size_t nb = (e1 - e0 + 1 + 7) / 8; std::vector<uint8_t> bm(nb, 0), esc; uint32_t ne = 0, li = 0; uint64_t pt = 0;
  for (uint32_t i = 0; i < n; i++) { size_t k; if (D.has(t[i], k) && k >= e0 && k <= e1) bm[(k - e0) >> 3] |= 1 << ((k - e0) & 7); else { wcb::putVar(esc, i - li); wcb::putVar(esc, t[i] - pt); li = i; pt = t[i]; ne++; } }
  wcb::putVar(o, e1 - e0 + 1); wcb::putVar(o, ne); o.insert(o.end(), esc.begin(), esc.end()); o.insert(o.end(), bm.begin(), bm.end());
}
// decode a block time header of kind tk into t[0..n), returns pointer past it
static const uint8_t *decodeTimes(const Domain *D, int tk, uint64_t t0, uint64_t firstEdge, uint32_t n, const uint8_t *p, uint64_t *t) {
  if (tk == TK_DT) { uint64_t c = t0; for (uint32_t i = 0; i < n; i++) { c += wcb::getVar(p); t[i] = c; } return p; }
  if (tk == TK_IDX) { size_t pi = firstEdge; uint64_t pt = t0;
    for (uint32_t i = 0; i < n; i++) { uint64_t c = wcb::getVar(p); if (c & 1) { pt += c >> 1; pi = D->idxOf(pt); } else { pi += c >> 1; pt = D->e[pi]; } t[i] = pt; } return p; }
  // bitmap
  uint64_t ne_edges = wcb::getVar(p), ne = wcb::getVar(p); std::vector<std::pair<uint32_t, uint64_t>> esc(ne); uint32_t li = 0; uint64_t pt = 0;
  for (uint64_t j = 0; j < ne; j++) { li += wcb::getVar(p); pt += wcb::getVar(p); esc[j] = {li, pt}; }
  const uint8_t *bm = p; p += (ne_edges + 7) / 8; size_t k = 0, ej = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (ej < ne && esc[ej].first == i) { t[i] = esc[ej++].second; continue; }
    while (k < ne_edges && !(bm[k >> 3] >> (k & 7) & 1)) k++;
    t[i] = D->e[firstEdge + k]; k++;
  }
  return p;
}
}  // namespace wdom
