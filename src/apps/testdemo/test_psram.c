#include "test.h"
#include "of_mixer.h"
#include <time.h>

/* Legacy PSRAM / SRAM probes.
 *
 * v2 memory arch retires CRAM1 and CPU-visible SRAM. The active test
 * path skips these probes; the old address constants remain below only
 * so the pre-v2 reference code still compiles when the skip is removed
 * for hardware archaeology. Current mixer samples and I/O cache data
 * live in SDRAM, not CRAM1. */

/* SDRAM region used to evict D-cache lines (must not overlap app code/data) */

void test_psram_memory(void) {
    section_start("PSRAM Mem");

    /* v2 memory arch retires CRAM1 + SRAM as CPU-addressable memory.
     * CRAM0 is kept but its only role is bridge staging (load/save
     * scratch), time-sliced with the CPU via an MMIO owner bit.
     * The old uncached aliases 0x38xxxxxx / 0x39xxxxxx / 0x3Axxxxxx
     * are no longer in any PMA region — a direct load/store traps.
     *
     * Skip the whole PSRAM memory test suite here: none of the
     * hardware it was probing exists as an app-addressable region
     * anymore.  The per-chip memory controllers are already exercised
     * by the standalone Verilator harnesses in src/fpga/test/. */
    test_pass("skipped (v2)");
    section_end();
    return;
}

void test_cram0_256k(void) {
    section_start("CRAM0 256K");

    /* v2: skipped.  See note in test_psram_memory(). */
    test_pass("skipped (v2)");
    section_end();
    return;
}
