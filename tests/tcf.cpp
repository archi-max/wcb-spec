// tcf: round-trip check for split.h changed-field coding (CFEnc): random packed structs, 2- and 4-state,
// multiple blocks; the decoded hash chain must equal the hash chain of the input values.
#include "../src/split.h"
#include <cstdio>
#include <random>
using namespace wsplit;
int main() {
  std::mt19937_64 r(777); int fails = 0, cases = 0;
  for (int iter = 0; iter < 300; iter++) {
    int nf = 1 + r() % 40; std::vector<Field> F; int p = 0;
    for (int f = 0; f < nf; f++) { int w = 1 + r() % 64; F.push_back({p, w, "f"}); p += w; }
    int sb = (p + 7) / 8; bool fs = r() % 2; int vlen = fs ? 2 * sb : sb;
    uint32_t blkN = 1 + r() % 20000; uint32_t n = 1 + r() % 60000;
    CFEnc cf; cf.init(F, sb, fs, 3, blkN);
    std::vector<uint8_t> v(vlen, 0); uint64_t h = 1469598103934665603ull, t = r() % 100;
    double busy = (double)(r() % 100) / 100.0;
    for (uint32_t i = 0; i < n; i++) {
      for (int f = 0; f < nf; f++) if ((double)(r() % 1000) / 1000.0 < busy) {
        uint64_t m = F[f].w == 64 ? ~0ull : ((1ull << F[f].w) - 1), a = r() & m, x = fs && r() % 8 == 0 ? (r() & m) : 0;
        for (int b = 0; b < F[f].w; b++) { int pos = F[f].pos + b; uint8_t bit = (a >> (F[f].w - 1 - b)) & 1;  // fill the field's bits
          v[pos >> 3] = (v[pos >> 3] & ~(1 << (pos & 7))) | (bit << (pos & 7));
          if (fs) { uint8_t xb = (x >> (F[f].w - 1 - b)) & 1; v[sb + (pos >> 3)] = (v[sb + (pos >> 3)] & ~(1 << (pos & 7))) | (xb << (pos & 7)); } }
      }
      cf.change(t, v.data()); h = wcb::hmix(h, t, v.data(), vlen); t += 1 + r() % 50;
    }
    cf.flush(); uint64_t nOut = 0, hd = cf.verify(vlen, nOut); cases++;
    if (hd != h || nOut != n) { if (fails < 5) fprintf(stderr, "FAIL iter %d nf %d fs %d n %u got %llu\n", iter, nf, fs, n, (unsigned long long)nOut); fails++; }
  }
  printf("tcf: %d cases, %d failures\n", cases, fails); return fails != 0;
}
