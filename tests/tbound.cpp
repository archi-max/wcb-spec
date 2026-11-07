// tbound: same-time changes across a block boundary (blkN=4; times 10,20,30,50 | 50,50,60,70).
// 1) inclusive change scan [t1,t2) from View::blockAt(t1) (old) vs View::blockFirst(t1) (fixed): expect 4 changes in [50,61)
// 2) collapsed window (value in effect at a, then changes in (a,b]) from blockAt(a): compared with brute force over all changes
#include "../src/wcb.h"
#include <cstdio>
#include <vector>
using namespace wcb;
static uint32_t scanIncl(ZSTD_DCtx *zd, const View &V, uint64_t t1, uint64_t t2, bool fixed) {
  std::vector<uint8_t> tmp; Decoded D; uint32_t n = 0;
  for (uint32_t b = fixed ? V.blockFirst(t1) : V.blockAt(t1); b < V.h->nblk && V.blk[b].t0 < t2; b++) { decodeBlock(zd, V, b, D, tmp); for (uint32_t i = 0; i < D.n; i++) if (D.t[i] >= t1 && D.t[i] < t2) n++; }
  return n;
}
struct C { int init; std::vector<std::pair<uint64_t, int>> ch; bool operator==(const C &o) const { return init == o.init && ch == o.ch; } };
static C collapsed(const std::vector<uint64_t> &T, const std::vector<int> &Vv, uint64_t a, uint64_t b) { C c{-1, {}};
  for (size_t i = 0; i < T.size(); i++) { if (T[i] <= a) { c.init = Vv[i]; continue; } if (T[i] > b) break; int last = c.ch.empty() ? c.init : c.ch.back().second;
    if (!c.ch.empty() && c.ch.back().first == T[i]) { c.ch.back().second = Vv[i]; int prev = c.ch.size() >= 2 ? c.ch[c.ch.size() - 2].second : c.init; if (prev == Vv[i]) c.ch.pop_back(); continue; }
    if (Vv[i] != last) c.ch.push_back({T[i], Vv[i]}); }
  return c; }
int main() {
  std::vector<uint64_t> T = {10, 20, 30, 50, 50, 50, 60, 70}; std::vector<int> Vv = {1, 2, 3, 4, 5, 6, 7, 8};
  Encoder E; E.init(8, 1, false, 100, 0, 3, 4); for (size_t i = 0; i < T.size(); i++) { uint8_t v = Vv[i]; E.change(T[i], &v); } auto img = E.finish();
  View V; V.open(img.data(), img.size()); ZSTD_DCtx *zd = ZSTD_createDCtx(); int fail = 0;
  printf("blocks %u: t0 = %llu, %llu\n", V.h->nblk, (unsigned long long)V.blk[0].t0, (unsigned long long)V.blk[1].t0);
  uint32_t o = scanIncl(zd, V, 50, 61, false), f = scanIncl(zd, V, 50, 61, true);
  printf("inclusive scan [50,61): blockAt start %u, blockFirst start %u (expected 4)\n", o, f); if (f != 4) fail++;
  // collapsed windows from blockAt(a), as wcfair storedWin/storedWin2 and the evaluator cursors do
  std::vector<uint8_t> tmp; Decoded D; int nw = 0, bad = 0;
  for (uint64_t a = 0; a <= 80; a += 5) for (uint64_t b = a; b <= 80; b += 5) {
    C ref = collapsed(T, Vv, a, b); std::vector<uint64_t> T2; std::vector<int> V2;
    for (uint32_t k = a ? V.blockAt(a) : 0; k < V.h->nblk && V.blk[k].t0 <= b; k++) { decodeBlock(zd, V, k, D, tmp); for (uint32_t i = 0; i < D.n; i++) { T2.push_back(D.t[i]); V2.push_back(D.v[i]); } }
    C got = collapsed(T2, V2, a, b); nw++; if (!(got == ref)) { bad++; printf("collapsed MISMATCH a=%llu b=%llu\n", (unsigned long long)a, (unsigned long long)b); } }
  printf("collapsed windows from blockAt(a): %d/%d match brute force\n", nw - bad, nw); if (bad) fail++;
  printf(fail ? "FAIL\n" : "PASS\n"); return fail;
}
