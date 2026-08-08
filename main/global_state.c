/**
 * global_state.c — see global_state.h.
 */

#include <string.h>
#include "global_state.h"
#include "board_config.h"

global_state_t g_state;

void global_state_init(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.mutex = xSemaphoreCreateMutex();
    g_state.next_job_id = 0;
    g_state.current_frequency_mhz = ASIC_DEFAULT_FREQUENCY_MHZ;
    g_state.current_voltage_mv = ASIC_DEFAULT_VOLTAGE_MV;
    /* Rough expected-hashrate estimate for display purposes only (not
     * used anywhere in the mining logic itself): small-core-count *
     * frequency is the standard back-of-envelope GH/s formula for a
     * single BM1397, but ASIC_SMALL_CORE_COUNT is itself an unverified
     * placeholder - see board_config.h. */
    g_state.expected_hashrate_ghs = (ASIC_SMALL_CORE_COUNT * ASIC_DEFAULT_FREQUENCY_MHZ) / 1000.0f;
}
