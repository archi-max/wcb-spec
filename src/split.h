// split.h: wide-struct codecs over the wcb study format (lossless, hash-verified).
//   Values arrive packed as in wcx: value plane then xz plane, bit i (MSB-first position i) at byte i>>3, bit i&7.
//   Fields are [pos, width) ranges in that MSB-first position space, in declaration order, each <= 64 bits.
// Variants per struct:
//   BASE   whole-struct wcb::Encoder (the Task B baseline)
//   SPLIT  one wcb::Encoder per field, fed only when that field changes, plus an "event" list of struct
//          changes where no field changed; field streams carry their own hash for cross-field/cross-signal dedupe
//   CF     changed-field coding: one stream, per change varint dt + field mask + XOR of the changed fields only,
//          laid out column-wise per block (all masks, then each field's XORs), zstd per 16K changes
#pragma once
#include "wcb.h"
#include <chrono>
#include <string>
#include <cstdlib>
namespace wsplit {
static double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Field { int pos, w; std::string name; };
static inline uint64_t getbits(const uint8_t *pl, int nbytes, int p, int w) {
  // bits [p, p+w) of a little-endian bit string, bit p -> result bit 0
  uint64_t r = 0; int b0 = p >> 3, sh = p & 7, need = (sh + w + 7) >> 3;
  uint8_t tmp[16] = {0}; for (int i = 0; i < need && b0 + i < nbytes; i++) tmp[i] = pl[b0 + i];
  uint64_t lo; memcpy(&lo, tmp, 8); r = lo >> sh; if (sh + w > 64) r |= (uint64_t)tmp[8] << (64 - sh);
  return w == 64 ? r : (r & ((1ull << w) - 1));
}
static inline void putbits(uint8_t *pl, int p, int w, uint64_t v) {  // OR v's low w bits in at bit p (destination zeroed)
  if (!v) return; int b0 = p >> 3, sh = p & 7, nb = (sh + w + 7) >> 3; uint64_t lo = v << sh;
  for (int i = 0; i < nb && i < 8; i++) pl[b0 + i] |= (uint8_t)(lo >> (8 * i));
  if (nb > 8) pl[b0 + 8] |= (uint8_t)(v >> (64 - sh));
}

struct CFEnc {  // changed-field coder
  int nf = 0, sb = 0, nmb = 0; std::vector<Field> F; bool fs = true; int level = 3; uint32_t blkN = 16384;
  std::vector<uint64_t> bt; std::vector<uint8_t> masks; std::vector<std::vector<uint64_t>> xv, xx; bool bx = false;
  std::vector<uint64_t> pv, px; std::vector<uint8_t> data; std::vector<wcb::BlkEnt> blks; ZSTD_CCtx *zc = nullptr; std::vector<uint8_t> cb;
  uint64_t n = 0;
  void init(const std::vector<Field> &f, int sbytes, bool fourState, int zl, uint32_t bn) {
    F = f; nf = f.size(); sb = sbytes; fs = fourState; level = zl; blkN = bn; nmb = (nf + 7) / 8;
    xv.assign(nf, {}); xx.assign(nf, {}); pv.assign(nf, 0); px.assign(nf, 0); if (!zc) zc = ZSTD_createCCtx();
  }
  void flush() {
    uint32_t m = bt.size(); if (!m) return;
    std::vector<uint8_t> raw; uint64_t tp = bt[0]; for (uint32_t i = 0; i < m; i++) { wcb::putVar(raw, bt[i] - tp); tp = bt[i]; }
    raw.insert(raw.end(), masks.begin(), masks.end());
    for (int f = 0; f < nf; f++) { int nb = (F[f].w + 7) / 8; for (uint64_t v : xv[f]) for (int k = 0; k < nb; k++) raw.push_back((uint8_t)(v >> (8 * k))); }
    if (fs && bx) for (int f = 0; f < nf; f++) { int nb = (F[f].w + 7) / 8; for (uint64_t v : xx[f]) for (int k = 0; k < nb; k++) raw.push_back((uint8_t)(v >> (8 * k))); }
    cb.resize(ZSTD_compressBound(raw.size())); size_t c = ZSTD_compressCCtx(zc, cb.data(), cb.size(), raw.data(), raw.size(), level);
    wcb::BlkEnt e{}; e.t0 = bt[0]; e.off = data.size(); e.csize = c; e.n = m; e.rsize = raw.size(); e.flags = (fs && !bx) ? wcb::F_2ST : 0;
    data.insert(data.end(), cb.begin(), cb.begin() + c); blks.push_back(e);
    bt.clear(); masks.clear(); for (int f = 0; f < nf; f++) { xv[f].clear(); xx[f].clear(); } bx = false;
    std::fill(pv.begin(), pv.end(), 0); std::fill(px.begin(), px.end(), 0);  // each block decodes alone
  }
  void change(uint64_t t, const uint8_t *v) {
    n++; bt.push_back(t); size_t mo = masks.size(); masks.resize(mo + nmb, 0);
    for (int f = 0; f < nf; f++) {
      uint64_t a = getbits(v, sb, F[f].pos, F[f].w), x = fs ? getbits(v + sb, sb, F[f].pos, F[f].w) : 0;
      if (a != pv[f] || x != px[f] || bt.size() == 1) { masks[mo + (f >> 3)] |= 1 << (f & 7); xv[f].push_back(a ^ pv[f]); xx[f].push_back(x ^ px[f]); if (x) bx = true; pv[f] = a; px[f] = x; }
    }
    if (bt.size() >= blkN) flush();
  }
  size_t bytes() const { return data.size() + blks.size() * sizeof(wcb::BlkEnt) + sizeof(wcb::Hdr); }
  // decode everything and return the hash chain (same as wcb Hdr.hash) over full-width packed values
  uint64_t verify(int vlen, uint64_t &nOut) const {
    ZSTD_DCtx *zd = ZSTD_createDCtx(); std::vector<uint8_t> tmp; uint64_t h = 1469598103934665603ull; nOut = 0;
    std::vector<uint8_t> cur(vlen); std::vector<size_t> fo(nf);
    for (auto &e : blks) {
      tmp.resize(e.rsize); ZSTD_decompressDCtx(zd, tmp.data(), e.rsize, data.data() + e.off, e.csize);
      const uint8_t *p = tmp.data(); std::vector<uint64_t> ts(e.n); uint64_t t = e.t0; for (uint32_t i = 0; i < e.n; i++) { t += wcb::getVar(p); ts[i] = t; }
      const uint8_t *mk = p; p += (size_t)e.n * nmb;
      std::vector<uint32_t> cnt(nf, 0); for (uint32_t i = 0; i < e.n; i++) for (int f = 0; f < nf; f++) if (mk[i * nmb + (f >> 3)] >> (f & 7) & 1) cnt[f]++;
      std::vector<const uint8_t *> pvp(nf), pxp(nf); for (int f = 0; f < nf; f++) { pvp[f] = p; p += (size_t)cnt[f] * ((F[f].w + 7) / 8); }
      bool has = !(e.flags & wcb::F_2ST) && fs; for (int f = 0; f < nf; f++) { pxp[f] = has ? p : nullptr; if (has) p += (size_t)cnt[f] * ((F[f].w + 7) / 8); }
      std::vector<uint64_t> a(nf, 0), x(nf, 0);
      for (uint32_t i = 0; i < e.n; i++) {
        for (int f = 0; f < nf; f++) if (mk[i * nmb + (f >> 3)] >> (f & 7) & 1) {
          int nb = (F[f].w + 7) / 8; uint64_t d = 0; for (int k = 0; k < nb; k++) d |= (uint64_t)pvp[f][k] << (8 * k); pvp[f] += nb; a[f] ^= d;
          if (pxp[f]) { d = 0; for (int k = 0; k < nb; k++) d |= (uint64_t)pxp[f][k] << (8 * k); pxp[f] += nb; x[f] ^= d; } }
        memset(cur.data(), 0, vlen); for (int f = 0; f < nf; f++) { putbits(cur.data(), F[f].pos, F[f].w, a[f]); if (fs) putbits(cur.data() + sb, F[f].pos, F[f].w, x[f]); }
        h = wcb::hmix(h, ts[i], cur.data(), vlen); nOut++;
      }
    }
    ZSTD_freeDCtx(zd); return h;
  }
};

struct StructCodec {
  std::vector<Field> F; int bits = 0, vlen = 0, sb = 0; bool fs = true;
  wcb::Encoder base; std::vector<wcb::Encoder> fe; CFEnc cf; std::vector<uint64_t> evt;  // struct changes with no field change
  std::vector<uint64_t> pa, px; uint64_t n = 0; bool first = true;
  double tBase = 0, tSplit = 0, tCF = 0;
  std::vector<uint8_t> fb;
  void init(const std::vector<Field> &f, int b, int vl, bool fourState, uint64_t tmax, uint64_t lodBase, int zl, uint32_t bn) {
    F = f; bits = b; vlen = vl; fs = fourState; sb = fs ? vl / 2 : vl;
    base.init(b, vl, fs, tmax, lodBase, zl, bn); fe.assign(F.size(), wcb::Encoder());
    uint64_t flb = getenv("WC_FIELDLOD") && atoi(getenv("WC_FIELDLOD")) ? lodBase : 0;  // per-field LOD off by default: the struct row keeps the struct-level LOD
    for (size_t i = 0; i < F.size(); i++) { int fb = (F[i].w + 7) / 8; fe[i].init(F[i].w, fs ? 2 * fb : fb, fs, tmax, flb, zl, bn); }
    cf.init(F, sb, fs, zl, bn); pa.assign(F.size(), 0); px.assign(F.size(), 0); n = 0; first = true; evt.clear();
  }
  void change(uint64_t t, const uint8_t *v) {
    n++;
    double a = nowS(); base.change(t, v); double b = nowS(); tBase += b - a;
    bool any = false; int nf = F.size();
    for (int i = 0; i < nf; i++) {
      uint64_t va = getbits(v, sb, F[i].pos, F[i].w), vx = fs ? getbits(v + sb, sb, F[i].pos, F[i].w) : 0;
      if (first || va != pa[i] || vx != px[i]) {
        int nb = (F[i].w + 7) / 8; fb.assign(fs ? 2 * nb : nb, 0);
        for (int k = 0; k < nb; k++) { fb[k] = (uint8_t)(va >> (8 * k)); if (fs) fb[nb + k] = (uint8_t)(vx >> (8 * k)); }
        fe[i].change(t, fb.data()); pa[i] = va; px[i] = vx; any = true; }
    }
    if (!any) evt.push_back(t);
    first = false; double c = nowS(); tSplit += c - b;
    cf.change(t, v); tCF += nowS() - c;
  }
  void finish() { cf.flush(); }
  // SPLIT round trip: decode every field image, merge by time with the event list, rebuild full values, hash.
  uint64_t verifySplit(std::vector<std::vector<uint8_t>> &imgs, uint64_t &nOut) {
    int nf = F.size(); ZSTD_DCtx *zd = ZSTD_createDCtx(); std::vector<uint8_t> tmp;
    std::vector<wcb::View> V(nf); std::vector<wcb::Decoded> D(nf); std::vector<uint32_t> bi(nf, 0), ii(nf, 0); std::vector<bool> live(nf, false);
    for (int i = 0; i < nf; i++) { V[i].open(imgs[i].data(), imgs[i].size()); if (V[i].h->nblk) { wcb::decodeBlock(zd, V[i], 0, D[i], tmp); live[i] = true; } }
    std::vector<uint64_t> a(nf, 0), x(nf, 0); std::vector<uint8_t> cur(vlen); uint64_t h = 1469598103934665603ull; nOut = 0; size_t ei = 0;
    for (;;) {
      uint64_t t = ~0ull; for (int i = 0; i < nf; i++) if (live[i] && D[i].t[ii[i]] < t) t = D[i].t[ii[i]];
      if (ei < evt.size() && evt[ei] < t) t = evt[ei];
      if (t == ~0ull) break;
      bool fromEvt = false;
      for (int i = 0; i < nf; i++) if (live[i] && D[i].t[ii[i]] == t) {
        int nb = (F[i].w + 7) / 8; const uint8_t *p = &D[i].v[(size_t)ii[i] * V[i].h->vlen]; uint64_t va = 0, vx = 0;
        for (int k = 0; k < nb; k++) { va |= (uint64_t)p[k] << (8 * k); if (fs) vx |= (uint64_t)p[nb + k] << (8 * k); }
        a[i] = va; x[i] = vx;
        if (++ii[i] >= D[i].n) { ii[i] = 0; if (++bi[i] < V[i].h->nblk) wcb::decodeBlock(zd, V[i], bi[i], D[i], tmp); else live[i] = false; } }
      if (ei < evt.size() && evt[ei] == t) { ei++; fromEvt = true; }
      (void)fromEvt;
      memset(cur.data(), 0, vlen); for (int i = 0; i < nf; i++) { putbits(cur.data(), F[i].pos, F[i].w, a[i]); if (fs) putbits(cur.data() + sb, F[i].pos, F[i].w, x[i]); }
      h = wcb::hmix(h, t, cur.data(), vlen); nOut++;
    }
    ZSTD_freeDCtx(zd); return h;
  }
};

// Field layouts from the design model (bigsoc_pkg.sv); declaration order = MSB first.
static std::vector<Field> layout(const std::vector<std::pair<const char *, int>> &decl) { std::vector<Field> f; int p = 0; for (auto &d : decl) { f.push_back({p, d.second, d.first}); p += d.second; } return f; }
static std::vector<std::pair<const char *, int>> tracePkt() {
  return {{"valid", 1}, {"core", 8}, {"order", 64}, {"insn", 32}, {"trap", 1}, {"rs1_addr", 5}, {"rs2_addr", 5}, {"rd_addr", 5}, {"rs1_rdata", 32}, {"rs2_rdata", 32},
          {"rd_wdata", 32}, {"pc_rdata", 32}, {"pc_wdata", 32}, {"mem_addr", 32}, {"mem_rmask", 4}, {"mem_wmask", 4}, {"mem_rdata", 32}, {"mem_wdata", 32}};
}
static std::vector<Field> fieldsFor(int bits) {
  std::vector<std::pair<const char *, int>> d;
  if (bits == 385) d = tracePkt();
  else if (bits == 512) {
    d = {{"hdr.kind", 4}, {"hdr.cl", 2}, {"hdr.tile", 2}, {"hdr.seq", 8}, {"hdr.drops", 16}, {"ts", 64}};
    auto p = tracePkt(); for (auto &x : p) d.push_back(x);
    d.push_back({"rsvd", 15}); d.push_back({"chk", 16});
  }
  return layout(d);
}
}  // namespace wsplit
