#include "nnue.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define NNUE_X86 1
#endif

// Net file layout (little endian):
//   char    magic[8] = "CHSNNUE1"
//   uint32  hidden
//   int32   QA, QB, SCALE
//   int16   ftWeights[768][hidden]      (real weight * QA)
//   int16   ftBias[hidden]              (real bias * QA)
//   int16   outWeights[2 * hidden]      (real weight * QB; first `hidden` = side to move, then the other side)
//   int32   outBias                     (real bias * QA * QB)
//
// Inference (all integer):  c = clamp(acc, 0, QA);  sum = sum(c * c * w);  sum /= QA;  sum += outBias;
//                           score = sum * SCALE / (QA * QB)     (divisions truncate toward zero)

namespace nnue {

namespace {
alignas(64) int16_t g_ftW[(size_t)NUM_FEATURES * MAX_HIDDEN];
alignas(64) int16_t g_ftB[MAX_HIDDEN];
alignas(64) int16_t g_outW[2 * MAX_HIDDEN];
int32_t g_outB = 0;
int  g_hidden = 0, g_qa = 255, g_qb = 64, g_scale = 400;
bool g_loaded = false;
bool g_avx2 = false;         // this CPU can run the AVX2 paths (checked once, at load)
bool g_simdAllowed = true;   // tests can switch the SIMD paths off to compare against the plain C++ code
bool g_fastEval = false;     // the loaded net satisfies the limits of the madd-based evaluation (see load())

bool fail(std::string* err, const char* msg) { if (err) *err = msg; return false; }
}  // namespace

bool load(const std::string& path, std::string* err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail(err, "cannot open net file");
    char magic[8]; uint32_t hidden = 0; int32_t qa = 0, qb = 0, scale = 0;
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "CHSNNUE1", 8) != 0 ||
        std::fread(&hidden, 4, 1, f) != 1 || std::fread(&qa, 4, 1, f) != 1 ||
        std::fread(&qb, 4, 1, f) != 1 || std::fread(&scale, 4, 1, f) != 1) {
        std::fclose(f);
        return fail(err, "bad net header");
    }
    if (hidden == 0 || hidden > (uint32_t)MAX_HIDDEN || qa <= 0 || qb <= 0 || scale <= 0) {
        std::fclose(f);
        return fail(err, "unsupported net parameters");
    }
    // read into temporaries first so a bad file never clobbers a working net
    static int16_t tW[(size_t)NUM_FEATURES * MAX_HIDDEN], tB[MAX_HIDDEN], tO[2 * MAX_HIDDEN];
    int32_t tOB = 0;
    const size_t nW = (size_t)NUM_FEATURES * hidden, nO = 2 * (size_t)hidden;
    bool ok = std::fread(tW, 2, nW, f) == nW && std::fread(tB, 2, hidden, f) == hidden &&
              std::fread(tO, 2, nO, f) == nO && std::fread(&tOB, 4, 1, f) == 1;
    const bool extra = ok && std::fgetc(f) != EOF;      // trailing bytes: wrong file
    std::fclose(f);
    if (!ok || extra) return fail(err, "net file has the wrong size");

    std::memcpy(g_ftW, tW, nW * 2);
    std::memcpy(g_ftB, tB, (size_t)hidden * 2);
    std::memcpy(g_outW, tO, nO * 2);
    g_outB = tOB; g_hidden = (int)hidden; g_qa = qa; g_qb = qb; g_scale = scale;
#ifdef NNUE_X86
    __builtin_cpu_init();
    g_avx2 = __builtin_cpu_supports("avx2");
#endif
    // The AVX2 evaluation computes (c * w) as int16 and (c * w) * c as int32 lane sums. That is exact (identical to
    // the plain code) when |w| * QA fits in int16 and the lane sums cannot overflow int32. Check both here.
    int maxW = 0;
    for (size_t i = 0; i < nO; ++i) maxW = std::max(maxW, std::abs((int)g_outW[i]));
    const int64_t perLane = (int64_t)(2 * (int64_t)hidden / 16) * 2 * (int64_t)maxW * qa * qa;   // madd iterations * 2 products
    g_fastEval = (hidden % 16 == 0) && (int64_t)maxW * qa <= 32767 && perLane < ((int64_t)1 << 31);
    g_loaded = true;
    return true;
}

void unload()        { g_loaded = false; }
bool loaded()        { return g_loaded; }
int  hiddenSize()    { return g_hidden; }

void reset(Accumulator& acc) {
    for (int p = 0; p < 2; ++p)
        std::memcpy(acc.v[p], g_ftB, (size_t)g_hidden * sizeof(int16_t));
}

void copy(Accumulator& dst, const Accumulator& src) {
    for (int p = 0; p < 2; ++p)
        std::memcpy(dst.v[p], src.v[p], (size_t)g_hidden * sizeof(int16_t));
}

#ifdef NNUE_X86
__attribute__((target("avx2"))) static void addAVX2(int16_t* a, const int16_t* w, int n) {
    for (int i = 0; i < n; i += 16)
        _mm256_storeu_si256((__m256i*)(a + i), _mm256_add_epi16(_mm256_loadu_si256((const __m256i*)(a + i)),
                                                                _mm256_loadu_si256((const __m256i*)(w + i))));
}
__attribute__((target("avx2"))) static void subAVX2(int16_t* a, const int16_t* w, int n) {
    for (int i = 0; i < n; i += 16)
        _mm256_storeu_si256((__m256i*)(a + i), _mm256_sub_epi16(_mm256_loadu_si256((const __m256i*)(a + i)),
                                                                _mm256_loadu_si256((const __m256i*)(w + i))));
}
#endif

void addFeature(Accumulator& acc, int perspective, int feature) {
    const int16_t* w = g_ftW + (size_t)feature * g_hidden;
    int16_t* a = acc.v[perspective];
#ifdef NNUE_X86
    if (g_avx2 && g_simdAllowed && g_hidden % 16 == 0) { addAVX2(a, w, g_hidden); return; }
#endif
    for (int i = 0; i < g_hidden; ++i) a[i] = (int16_t)(a[i] + w[i]);
}

void subFeature(Accumulator& acc, int perspective, int feature) {
    const int16_t* w = g_ftW + (size_t)feature * g_hidden;
    int16_t* a = acc.v[perspective];
#ifdef NNUE_X86
    if (g_avx2 && g_simdAllowed && g_hidden % 16 == 0) { subAVX2(a, w, g_hidden); return; }
#endif
    for (int i = 0; i < g_hidden; ++i) a[i] = (int16_t)(a[i] - w[i]);
}

#ifdef NNUE_X86
// sum over i of clamp(a[i], 0, qa)^2 * w[i], computed as madd((c * w) as int16, c): exact when g_fastEval holds.
__attribute__((target("avx2"))) static __m256i dotSideAVX2(const int16_t* a, const int16_t* w, int n, int qa, __m256i acc) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i vqa  = _mm256_set1_epi16((int16_t)qa);
    for (int i = 0; i < n; i += 16) {
        __m256i c  = _mm256_min_epi16(_mm256_max_epi16(_mm256_loadu_si256((const __m256i*)(a + i)), zero), vqa);
        __m256i cw = _mm256_mullo_epi16(c, _mm256_loadu_si256((const __m256i*)(w + i)));
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(cw, c));
    }
    return acc;
}

__attribute__((target("avx2"))) static int64_t sumAVX2(const int16_t* us, const int16_t* them, const int16_t* wUs,
                                                       const int16_t* wTh, int n, int qa) {
    __m256i acc = dotSideAVX2(us, wUs, n, qa, _mm256_setzero_si256());
    acc = dotSideAVX2(them, wTh, n, qa, acc);
    alignas(32) int32_t lanes[8];
    _mm256_store_si256((__m256i*)lanes, acc);
    int64_t t = 0;
    for (int i = 0; i < 8; ++i) t += lanes[i];
    return t;
}
#endif

void setSimd(bool on) { g_simdAllowed = on; }
bool simdActive()     { return g_avx2 && g_simdAllowed; }

int evaluate(const Accumulator& acc, int stm) {
    const int16_t* us   = acc.v[stm];
    const int16_t* them = acc.v[stm ^ 1];
    const int16_t* wUs  = g_outW;
    const int16_t* wTh  = g_outW + g_hidden;
    const int qa = g_qa;
#ifdef NNUE_X86
    if (g_avx2 && g_simdAllowed && g_fastEval) {
        int64_t sum = sumAVX2(us, them, wUs, wTh, g_hidden, qa);
        sum /= qa;
        sum += g_outB;
        return (int)(sum * g_scale / ((int64_t)qa * g_qb));
    }
#endif
    int64_t sum = 0;
    for (int i = 0; i < g_hidden; ++i) {
        const int cu = std::max(0, std::min((int)us[i], qa));
        const int ct = std::max(0, std::min((int)them[i], qa));
        sum += (int64_t)((int32_t)(cu * cu) * (int32_t)wUs[i]);
        sum += (int64_t)((int32_t)(ct * ct) * (int32_t)wTh[i]);
    }
    sum /= qa;
    sum += g_outB;
    return (int)(sum * g_scale / ((int64_t)qa * g_qb));
}

}  // namespace nnue