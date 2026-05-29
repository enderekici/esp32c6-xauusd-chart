#pragma once
#include <stdint.h>

// Spleen 8x16 by Frederic Cambus — BSD-2-Clause.
// 95 glyphs (ASCII 0x20..0x7E). Row-major, 16 bytes per glyph,
// bit 7 = leftmost pixel. Cell 8x16. Native design size; not a 2x scale-up.
extern const uint8_t font_spleen8x16[95][16];
