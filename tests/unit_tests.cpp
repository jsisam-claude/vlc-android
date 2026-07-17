// Unit tests for the engine's pure (platform-free) helpers. Built and run
// in CI as a gate before the artifacts are trusted. No Windows headers, so
// it compiles and runs anywhere; the same source is exercised by the real
// engine (src/pixops.h is included by src/player.cpp).
#include "../src/pixops.h"

#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

// A 2x3 image (w=2,h=3), values encode (row*10+col) so rotations are
// easy to reason about:
//   00 01
//   10 11
//   20 21
static std::vector<uint32_t> sample(int w, int h) {
    std::vector<uint32_t> v((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) v[(size_t)y * w + x] = (uint32_t)(y * 10 + x);
    return v;
}

static void test_identity() {
    auto in = sample(2, 3);
    std::vector<uint32_t> out;
    int ow = 0, oh = 0;
    rotate_bgra(in.data(), 2, 3, 0, out, &ow, &oh);
    CHECK(ow == 2 && oh == 3);
    CHECK(out == in);
}

static void test_dims_swap() {
    auto in = sample(2, 3);
    std::vector<uint32_t> out;
    int ow = 0, oh = 0;
    rotate_bgra(in.data(), 2, 3, 90, out, &ow, &oh);
    CHECK(ow == 3 && oh == 2);
    rotate_bgra(in.data(), 2, 3, 270, out, &ow, &oh);
    CHECK(ow == 3 && oh == 2);
    rotate_bgra(in.data(), 2, 3, 180, out, &ow, &oh);
    CHECK(ow == 2 && oh == 3);
}

static void test_90() {
    // CW 90 of the 2x3 becomes 3x2:
    //   20 10 00
    //   21 11 01
    auto in = sample(2, 3);
    std::vector<uint32_t> out;
    int ow = 0, oh = 0;
    rotate_bgra(in.data(), 2, 3, 90, out, &ow, &oh);
    const uint32_t want[] = {20, 10, 0, 21, 11, 1};
    CHECK(out.size() == 6);
    for (int i = 0; i < 6; i++) CHECK(out[i] == want[i]);
}

static void test_180() {
    // 180 reverses row and column order.
    auto in = sample(2, 3);
    std::vector<uint32_t> out;
    int ow = 0, oh = 0;
    rotate_bgra(in.data(), 2, 3, 180, out, &ow, &oh);
    const uint32_t want[] = {21, 20, 11, 10, 1, 0};
    for (int i = 0; i < 6; i++) CHECK(out[i] == want[i]);
}

static void test_360_wraps() {
    // Out-of-range and negative angles normalize to the 0..270 quadrants.
    auto in = sample(2, 3);
    std::vector<uint32_t> a, b;
    int ow = 0, oh = 0;
    rotate_bgra(in.data(), 2, 3, 360, a, &ow, &oh);
    CHECK(a == in);
    rotate_bgra(in.data(), 2, 3, -90, a, &ow, &oh);
    rotate_bgra(in.data(), 2, 3, 270, b, &ow, &oh);
    CHECK(a == b);
}

static void test_roundtrip() {
    // Four 90-degree rotations return the original.
    auto in = sample(5, 3);
    std::vector<uint32_t> cur = in, next;
    int ow = 0, oh = 0, w = 5, h = 3;
    for (int i = 0; i < 4; i++) {
        rotate_bgra(cur.data(), w, h, 90, next, &ow, &oh);
        cur = next;
        w = ow;
        h = oh;
    }
    CHECK(w == 5 && h == 3);
    CHECK(cur == in);
}

int main() {
    test_identity();
    test_dims_swap();
    test_90();
    test_180();
    test_360_wraps();
    test_roundtrip();
    if (g_fail) {
        printf("%d test(s) failed\n", g_fail);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
