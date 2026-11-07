// wcbcheck: validate WCB files end to end with a given wcb.h reader.
//   View::open, decode every block and LOD chunk, recompute Hdr.hash over all (t, value), check n and t0/tlast.
// Build: g++ -O2 -std=c++17 -DWCB_H='"path/to/wcb.h"' tools/wcbcheck.cpp -lzstd -o wcbcheck
// Usage: wcbcheck file.wcb...   (exit 1 if any file fails)
#ifndef WCB_H
#define WCB_H "../src/wcb.h"
#endif
#include WCB_H
#include <cstdio>
#include <fstream>
#include <iterator>
#include <type_traits>
using namespace wcb;
// older readers return void from decodeBlock/decodeLod (no validation); treat that as success
template <class F> static bool run(F f) { if constexpr (std::is_void_v<decltype(f())>) { f(); return true; } else return f(); }
int main(int argc, char **argv) {
  ZSTD_DCtx *zd = ZSTD_createDCtx(); int bad = 0, good = 0; uint64_t changes = 0;
  for (int a = 1; a < argc; a++) {
    std::ifstream in(argv[a], std::ios::binary); std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), {});
    View V; const char *why = nullptr; Decoded D; std::vector<uint8_t> tmp; std::vector<LodItem> L;
    if (!V.open(f.data(), f.size())) why = "open";
    uint64_t h = 1469598103934665603ull, n = 0, tf = 0, tl = 0;
    for (uint32_t b = 0; !why && b < V.h->nblk; b++) {
      bool r = run([&] { return decodeBlock(zd, V, b, D, tmp); });
      if (!r || D.n != V.blk[b].n) { why = "block decode"; break; }
      for (uint32_t i = 0; i < D.n; i++, n++) { if (!n) tf = D.t[i]; tl = D.t[i]; h = hmix(h, D.t[i], &D.v[(size_t)i * V.h->vlen], V.h->vlen); }
    }
    for (uint32_t c = 0; !why && c < V.h->nchunk; c++) {
      bool r = run([&] { return decodeLod(zd, V, c, L, tmp); });
      if (!r) why = "lod decode";
    }
    if (!why && n != V.h->n) why = "change count";
    if (!why && n && (tf != V.h->t0 || tl != V.h->tlast)) why = "t0/tlast";
    if (!why && h != V.h->hash) why = "stream hash";
    if (why) { printf("FAIL %s: %s\n", argv[a], why); bad++; } else { good++; changes += n; }
  }
  printf("%d ok, %d failed, %llu changes\n", good, bad, (unsigned long long)changes);
  return bad || !good;
}
