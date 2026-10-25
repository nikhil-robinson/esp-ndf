/* Bounded busy-waits: every wait on hardware ends, so a stuck peripheral (clock off, held in reset, bus dead) can
 * never hang the caller. SPIN_US(cond, us) spins until cond is true or about us microseconds passed (counted at
 * 240 MHz: at lower clocks the bound is longer, never shorter); value: cond at the end. */
#pragma once
#include "ndf.h"
#include "xtensa.h"

#define SPIN_US(cond, us) ({ uint32_t _t0 = ccount(), _lim = (uint32_t)(us) * 240u; bool _ok;           \
    while (!(_ok = (cond)) && ccount() - _t0 < _lim) {} _ok; })
