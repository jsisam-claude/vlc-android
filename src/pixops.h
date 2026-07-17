// Pure pixel helpers, free of platform headers so they can be unit-tested
// on any toolchain (see tests/unit_tests.cpp).
#pragma once
#include <cstdint>
#include <vector>

// Rotate a packed 32-bit-per-pixel image clockwise by 0/90/180/270 degrees.
// Format-agnostic (BGRA or RGBA - pixels are copied whole). Writes the
// rotated image to out and reports the new dimensions.
inline void rotate_bgra(const uint32_t* in, int w, int h, int rot,
                        std::vector<uint32_t>& out, int* ow_out, int* oh_out) {
    rot = ((rot % 360) + 360) % 360;
    int ow = (rot == 90 || rot == 270) ? h : w;
    int oh = (rot == 90 || rot == 270) ? w : h;
    out.resize((size_t)ow * oh);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int sx, sy;
            if (rot == 90) { sx = y; sy = h - 1 - x; }
            else if (rot == 270) { sx = w - 1 - y; sy = x; }
            else if (rot == 180) { sx = w - 1 - x; sy = h - 1 - y; }
            else { sx = x; sy = y; }
            out[(size_t)y * ow + x] = in[(size_t)sy * w + sx];
        }
    *ow_out = ow;
    *oh_out = oh;
}
