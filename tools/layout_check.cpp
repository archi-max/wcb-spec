// Prints the on-disk struct sizes/offsets and round-trips a small 4-state stream through wcb.h.
#include "../src/variants/wcb.base.h"
#include <cstdio>
#include <cstddef>
using namespace wcb;
#define O(S, f) printf("  %-10s %3zu\n", #f, offsetof(S, f))
int main() {
  printf("Hdr %zu\n", sizeof(Hdr)); O(Hdr,magic);O(Hdr,bits);O(Hdr,vlen);O(Hdr,fourState);O(Hdr,n);O(Hdr,t0);O(Hdr,tlast);O(Hdr,lodBase);O(Hdr,hash);O(Hdr,nblk);O(Hdr,nlev);O(Hdr,nchunk);O(Hdr,pad);
  printf("BlkEnt %zu\n", sizeof(BlkEnt)); O(BlkEnt,t0);O(BlkEnt,off);O(BlkEnt,csize);O(BlkEnt,n);O(BlkEnt,rsize);O(BlkEnt,mode);O(BlkEnt,flags);
  printf("LevEnt %zu\n", sizeof(LevEnt)); O(LevEnt,firstChunk);O(LevEnt,nchunk);O(LevEnt,entries);
  printf("LodEnt %zu\n", sizeof(LodEnt)); O(LodEnt,b0);O(LodEnt,off);O(LodEnt,csize);O(LodEnt,n);O(LodEnt,rsize);
  // 12-bit 4-state signal: vlen = 2 * 2 bytes; 40,000 changes -> 3 blocks of 16384; x on change 30000 only
  const int bits = 12, vl = 4; Encoder E; E.init(bits, vl, true, 400000, 10, 3, 16384);
  std::vector<std::vector<uint8_t>> ref; std::vector<uint64_t> rt;
  for (int i = 0; i < 40000; i++) { uint8_t v[4] = {(uint8_t)(i % 7), (uint8_t)((i / 7) & 0xf), 0, 0}; if (i == 30000) v[2] = 1; uint64_t t = 10ull * i + (i % 3 == 0); E.change(t, v); ref.emplace_back(v, v + 4); rt.push_back(t); }
  auto f = E.finish(); View V; if (!V.open(f.data(), f.size())) { puts("open FAIL"); return 1; }
  printf("file %zu bytes, nblk %u nlev %u nchunk %u\n", f.size(), V.h->nblk, V.h->nlev, V.h->nchunk);
  ZSTD_DCtx *zd = ZSTD_createDCtx(); Decoded D; std::vector<uint8_t> tmp; size_t k = 0;
  for (uint32_t b = 0; b < V.h->nblk; b++) { decodeBlock(zd, V, b, D, tmp); printf("  blk %u mode %s flags %u n %u\n", b, V.blk[b].mode ? "DICT" : "XOR", V.blk[b].flags, D.n);
    for (uint32_t i = 0; i < D.n; i++, k++) if (D.t[i] != rt[k] || memcmp(&D.v[i * vl], ref[k].data(), vl)) { printf("MISMATCH at %zu\n", k); return 1; } }
  printf("round trip OK (%zu changes)\n", k); return 0;
}
