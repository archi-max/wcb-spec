// bench.cpp: wcb::Encoder (today) vs wcbfast::Encoder on real WCB2 streams.
// usage: bench <zstd level> <blkN> <lod 0|1> <reps> files...
#include "../src/wcbfast.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <cstring>
#include <sys/stat.h>
#include <cstdlib>
struct Stream { int bits, vlen; bool fs; std::vector<uint64_t> t; std::vector<uint8_t> v; uint64_t lodBase, tlast, hash; std::string name; };
static bool load(const char *path, Stream &s) {
  struct stat st; if (stat(path, &st) || !S_ISREG(st.st_mode)) return false;
  std::ifstream f(path, std::ios::binary); std::vector<uint8_t> img{std::istreambuf_iterator<char>(f), {}};
  wcb::View V; if (img.size() < sizeof(wcb::Hdr) || !V.open(img.data(), img.size())) return false;
  s.bits = V.h->bits; s.vlen = V.h->vlen; s.fs = V.h->fourState; s.lodBase = V.h->lodBase; s.tlast = V.h->tlast; s.hash = V.h->hash; s.name = path;
  ZSTD_DCtx *zd = ZSTD_createDCtx(); std::vector<uint8_t> tmp; wcb::Decoded D;
  for (uint32_t b = 0; b < V.h->nblk; b++) { if (!wcb::decodeBlock(zd, V, b, D, tmp)) { ZSTD_freeDCtx(zd); return false; } s.t.insert(s.t.end(), D.t.begin(), D.t.end()); s.v.insert(s.v.end(), D.v.begin(), D.v.end()); }
  ZSTD_freeDCtx(zd);
  if (s.t.size() != V.h->n || s.v.size() != s.t.size() * (size_t)s.vlen) return false;  // decoded count must match the header
  uint64_t hh = 1469598103934665603ull; for (size_t i = 0; i < s.t.size(); i++) hh = wcb::hmix(hh, s.t[i], &s.v[i * s.vlen], s.vlen);
  return hh == V.h->hash;  // and so must the stream hash
}
static bool roundTrip(const std::vector<uint8_t> &img, const Stream &s) {
  wcb::View V; if (!V.open(img.data(), img.size())) return false; if (V.h->n != s.t.size() || V.h->hash != s.hash) return false;
  ZSTD_DCtx *zd = ZSTD_createDCtx(); std::vector<uint8_t> tmp; wcb::Decoded D; size_t k = 0; bool ok = true;
  for (uint32_t b = 0; b < V.h->nblk && ok; b++) { if (!wcb::decodeBlock(zd, V, b, D, tmp)) { ok = false; break; }
    for (uint32_t i = 0; i < D.n; i++, k++) if (D.t[i] != s.t[k] || memcmp(&D.v[(size_t)i * s.vlen], &s.v[k * s.vlen], s.vlen)) { ok = false; break; } }
  ZSTD_freeDCtx(zd); return ok && k == s.t.size();
}
// Returns the time for init+change+finish; *noInit gets change+finish only. Both are reported.
template <class E> static double enc(const Stream &s, int lvl, uint32_t bn, bool lod, std::vector<uint8_t> &out, E &e, double *noInit, size_t *peakState) {
  clock_t c0 = clock();
  e.init(s.bits, s.vlen, s.fs, s.tlast, lod ? (s.lodBase ? s.lodBase : 20000) : 0, lvl, bn);
  clock_t c1 = clock(); size_t n = s.t.size();
  for (size_t i = 0; i < n; i++) e.change(s.t[i], &s.v[i * s.vlen]);
  out = e.finish(); clock_t c2 = clock(); *noInit = double(c2 - c1) / CLOCKS_PER_SEC; return double(c2 - c0) / CLOCKS_PER_SEC;
}
int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage: bench <level> <blkN> <lod> <reps> files...  (POLICY=skip for SkipNoRepeat)\n"); return 2; }
  const bool skip = getenv("POLICY") && !strcmp(getenv("POLICY"), "skip"); int lvl = atoi(argv[1]); uint32_t bn = atoi(argv[2]); bool lod = atoi(argv[3]); int reps = atoi(argv[4]);
  double tA = 0, tB = 0; uint64_t nAll = 0, nWide = 0, szA = 0, szB = 0, same = 0, files = 0, rtFail = 0, modeDiff = 0, dictTried = 0, blocks = 0; double tAw = 0, tBw = 0, tAi = 0, tBi = 0; uint64_t skipped = 0, apiFail = 0; size_t peakState = 0, bytesBad = 0;
  for (int a = 5; a < argc; a++) {
    Stream s; if (!load(argv[a], s)) { skipped++; fprintf(stderr, "SKIPPED (unreadable) %s\n", argv[a]); continue; } files++;
    std::vector<uint8_t> oA, oB; double bestA = 1e9, bestB = 1e9, bestAn = 1e9, bestBn = 1e9, xn; uint64_t mA[2] = {0, 0}, mB[2] = {0, 0}, dt = 0, nb = 0;
    for (int r = 0; r < reps; r++) {
      size_t aBlk, aLod;
      { wcb::Encoder e; double x = enc(s, lvl, bn, lod, oA, e, &xn, nullptr); bestA = std::min(bestA, x); bestAn = std::min(bestAn, xn); mA[0] = e.modeCount[0]; mA[1] = e.modeCount[1]; aBlk = e.blkBytes(); aLod = e.lodBytes(); }
      { wcbfast::Encoder e; if (skip) e.policy = wcbfast::Policy::SkipNoRepeat;
        // memory probe: state just before finish() is the per-signal footprint
        double x = enc(s, lvl, bn, lod, oB, e, &xn, nullptr); bestB = std::min(bestB, x); bestBn = std::min(bestBn, xn); mB[0] = e.modeCount[0]; mB[1] = e.modeCount[1]; dt = e.dictTried; nb = e.blks.size();
        peakState = std::max(peakState, e.stateBytes());
        if (!skip && (e.blkBytes() != aBlk || e.lodBytes() != aLod)) { apiFail++; fprintf(stderr, "blkBytes/lodBytes MISMATCH %s\n", argv[a]); }
        wcbfast::Encoder m(std::move(e)); wcbfast::Encoder c = m; (void)c; }
    }
    tA += bestA; tB += bestB; tAi += bestAn; tBi += bestBn; if (!skip && oA != oB) bytesBad++; nAll += s.t.size(); szA += oA.size(); szB += oB.size(); same += (oA == oB); dictTried += dt; blocks += nb;
    if (mA[1] != mB[1]) { modeDiff++; if (getenv("SHOW")) fprintf(stderr, "mismatch bits=%d dictA=%llu dictB=%llu %s\n", s.bits, (unsigned long long)mA[1], (unsigned long long)mB[1], argv[a]); }
    if (s.bits > 64) { nWide += s.t.size(); tAw += bestA; tBw += bestB; }
    if (!roundTrip(oA, s) || !roundTrip(oB, s)) { rtFail++; fprintf(stderr, "ROUNDTRIP FAIL %s\n", argv[a]); }
  }
  printf("level=%d blkN=%u lod=%d files=%llu changes=%llu (wide %llu) blocks=%llu dictTried=%llu\n", lvl, bn, (int)lod, (unsigned long long)files, (unsigned long long)nAll, (unsigned long long)nWide, (unsigned long long)blocks, (unsigned long long)dictTried);
  printf("bytes  today=%llu  fast=%llu  (%+.3f%%)  byte-identical files %llu/%llu  mode-mismatch files %llu  roundtrip fails %llu\n", (unsigned long long)szA, (unsigned long long)szB, 100.0 * ((double)szB - szA) / szA, (unsigned long long)same, (unsigned long long)files, (unsigned long long)modeDiff, (unsigned long long)rtFail);
  if (!nAll) { fprintf(stderr, "FAIL: empty corpus\n"); return 1; }
  printf("policy=%s  skipped files %llu\n", skip ? "SkipNoRepeat" : "Preserve(default)", (unsigned long long)skipped);
  printf("encode us/change  all, init included: today %.3f  fast %.3f  (%.1fx)\n", tA * 1e6 / nAll, tB * 1e6 / nAll, tA / tB);
  printf("encode us/change  all, init excluded: today %.3f  fast %.3f  (%.1fx)\n", tAi * 1e6 / nAll, tBi * 1e6 / nAll, tAi / tBi);
  printf("memory: largest per-signal encoder state %.3f MiB; shared per-thread workspace %.3f MiB\n", peakState / 1048576.0, wcbfast::ws().bytes() / 1048576.0);
  if (nWide) printf("encode us/change  wide(>64b), init included: today %.3f  fast %.3f  (%.1fx)\n", tAw * 1e6 / nWide, tBw * 1e6 / nWide, tAw / tBw);
  int bad = rtFail || skipped || apiFail || (!skip && bytesBad);
  if (bad) fprintf(stderr, "FAIL: roundtrip %llu, skipped %llu, api %llu, non-identical (Preserve) %zu\n", (unsigned long long)rtFail, (unsigned long long)skipped, (unsigned long long)apiFail, bytesBad);
  else printf("PASS\n");
  return bad ? 1 : 0;
}
