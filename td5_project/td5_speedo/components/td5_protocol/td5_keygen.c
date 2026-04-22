/*
 * TD5 Keygen - verified against real Defender TD5 ECU.
 * Variable-count LFSR algorithm from pyTD5Tester/hairyone.
 */
#include "td5_keygen.h"

uint16_t td5_keygen(uint16_t seed)
{
    uint16_t s = seed & 0xFFFF;

    /* Iteration count derived from specific seed bits */
    int count = ((s >> 0xC & 0x8) + (s >> 0x5 & 0x4) +
                 (s >> 0x3 & 0x2) + (s & 0x1)) + 1;

    for (int i = 0; i < count; i++) {
        uint16_t tap = ((s >> 1) + (s >> 2) + (s >> 8) + (s >> 9)) & 1;
        uint16_t tmp = (s >> 1) | (tap << 0xF);

        if ((s >> 0x3 & 1) && (s >> 0xD & 1)) {
            s = tmp & (uint16_t)~1;
        } else {
            s = tmp | 1;
        }
    }
    return s & 0xFFFF;
}
