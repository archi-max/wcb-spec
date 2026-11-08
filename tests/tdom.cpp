// tdom: round-trip check for dom.h time headers (TK_DT, TK_IDX, TK_BITMAP), including off-edge escapes.
#include "../src/dom.h"
#include <cstdio>
#include <random>
using namespace wdom;
int main() {
  std::mt19937_64 r(12345); int fails = 0, cases = 0;
  for (int iter = 0; iter < 2000; iter++) {
    Domain D; uint64_t t = r() % 50, period = 1 + r() % 20;
    size_t ne = 1 + r() % 40000; for (size_t i = 0; i < ne; i++) { D.e.push_back(t); t += period + (r() % 4 == 0 ? r() % 7 : 0); }
    double pOn = (double)(r() % 101) / 100.0;           // fraction of changes on an edge
    std::vector<uint64_t> ts; uint64_t c = D.e[0] + (r() % 3 ? 0 : r() % 5);
    uint32_t n = 1 + r() % 16384;
    for (uint32_t i = 0; i < n && c <= D.e.back() + 1000; i++) {
      size_t k = D.idxOf(c); bool on = (double)(r() % 1000) / 1000.0 < pOn;
      uint64_t v = on && k < D.e.size() ? D.e[k] : c;
      if (!ts.empty() && v <= ts.back()) v = ts.back() + 1;
      ts.push_back(v); c = v + 1 + r() % (3 * period);
    }
    n = ts.size(); if (!n) continue;
    for (int tk : {TK_DT, TK_IDX, TK_BITMAP}) {
      std::vector<uint8_t> o; uint64_t fe = 0;
      if (tk == TK_DT) timeDT(ts.data(), n, o); else if (tk == TK_IDX) timeIDX(D, ts.data(), n, o, fe); else timeBMP(D, ts.data(), n, o, fe);
      std::vector<uint64_t> back(n); const uint8_t *p = decodeTimes(&D, tk, ts[0], fe, n, o.data(), back.data());
      cases++;
      if (back != ts || p != o.data() + o.size()) { if (fails < 5) fprintf(stderr, "FAIL iter %d tk %d n %u\n", iter, tk, n); fails++; }
    }
  }
  printf("tdom: %d cases, %d failures\n", cases, fails); return fails != 0;
}
