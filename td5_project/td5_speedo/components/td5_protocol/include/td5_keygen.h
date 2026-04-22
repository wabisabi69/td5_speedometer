/*
 * TD5 Keygen - Security key generation for Land Rover TD5 ECU diagnostics
 * Reference: https://github.com/pajacobson/td5keygen (BSD-2-Clause)
 */
#pragma once
#include <stdint.h>

uint16_t td5_keygen(uint16_t seed);
