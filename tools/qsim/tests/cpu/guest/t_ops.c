//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Guest driver for the generated instruction stubs.  The host (cpurun) picks
 * every vector through the test device at 0x4F000000 and checks each result
 * against its own reference; the guest only executes.
 */
#include <stdint.h>
#include "stubs_guest.h"

#define DEV 0x4F000000u
#define R(o) (*(volatile uint32_t *)(DEV + (o)))
#define W(o, v) (*(volatile uint32_t *)(DEV + (o)) = (v))

extern uint32_t trap_cause, trap_tval;

typedef uint32_t (*istub)(uint32_t, uint32_t);
typedef uint32_t (*fstub)(uint32_t, uint32_t, float, float, float);

/* CSR helpers (crt0.S) */
uint32_t get_fflags(void);
void set_frm_fflags(uint32_t frm, uint32_t fl);
void set_fcsr(uint32_t v);

static float bits2f(uint32_t b)
{
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

int main(void)
{
    for (uint32_t i = 0; i < NSTUBS; i++) {
        W(0x10, i);
        uint32_t nvec = R(0x14);
        for (uint32_t k = 0; k < nvec; k++) {
            uint32_t a = R(0x18), b = R(0x1c), c = R(0x20), d = R(0x24), ctl = R(0x28);
            uint32_t res, fl = 0;
            trap_cause = 0;
            if (stubs[i].fl) {
                uint32_t keep = (ctl & 1) ? 0 : get_fflags();
                set_frm_fflags(d, keep);
                res = ((fstub)stubs[i].fn)(a, 0, bits2f(a), bits2f(b), bits2f(c));
                fl = get_fflags();
                set_fcsr(fl);            /* frm back to 0, keep flags visible */
            } else {
                res = ((istub)stubs[i].fn)(a, b);
            }
            if (trap_cause) res = 0xdead0000u | trap_cause;
            W(0x30, fl);
            W(0x34, res);
        }
    }
    W(0x04, 0);
    return 0;
}
