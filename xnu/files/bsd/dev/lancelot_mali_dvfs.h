/* Portable MT6768 GPU OPP/clock math and demand policy; no hardware access.
 * Clock/rail facts: vendor mtk_gpufreq_core.c/.h (source hashes in the offline
 * evidence manifest). OPP subset independently observed on this Lancelot:
 * Android gpufreq_opp_dump SHA256 dcc7fc335129b1b371f2bfc7d7b8d3b5368a336a29f7cbb4d13d5aea820ec0df.
 * This is NOT proof of XNU thermal, battery or power-budget enforcement.
 */
#ifndef LANCELOT_MALI_DVFS_H
#define LANCELOT_MALI_DVFS_H
#include <stdbool.h>
#include <stdint.h>

#define LGD_OPP_COUNT 30u
#define LGD_MAX_KHZ 950000u
#define LGD_PLL_FIELDS UINT32_C(0x073fffff)
#define LGD_PLL_CHANGE UINT32_C(0x80000000)
#define LGD_RAIL_STEP_UV 6250u
#define LGD_DIFF_MIN_UV 100000u
#define LGD_DIFF_MAX_UV 250000u

struct lgd_opp { uint32_t khz, vgpu_uv, vsram_uv; };
static inline bool lgd_get_opp(uint32_t index, struct lgd_opp *opp)
{
    static const uint32_t khz[LGD_OPP_COUNT] = {
        950000,925000,900000,875000,850000,823000,796000,769000,
        743000,716000,690000,663000,637000,611000,586000,560000,
        535000,509000,484000,467000,450000,434000,417000,400000,
        383000,366000,349000,332000,315000,299000
    };
    static const uint32_t uv[LGD_OPP_COUNT] = {
        900000,875000,850000,825000,800000,793750,781250,768750,
        756250,750000,737500,725000,712500,706250,700000,693750,
        687500,681250,668750,668750,662500,656250,650000,643750,
        643750,637500,631250,625000,618750,612500
    };
    if (!opp || index >= LGD_OPP_COUNT) return false;
    opp->khz = khz[index]; opp->vgpu_uv = uv[index];
    opp->vsram_uv = uv[index] >= 750000u ? uv[index] + LGD_DIFF_MIN_UV : 850000u;
    return true;
}

static inline bool lgd_rail_pair_valid(uint32_t gpu, uint32_t sram)
{
    return gpu >= 612500u && gpu <= 950000u &&
        sram >= 850000u && sram <= 1050000u && sram >= gpu &&
        sram - gpu >= LGD_DIFF_MIN_UV && sram - gpu <= LGD_DIFF_MAX_UV &&
        (gpu - 500000u) % LGD_RAIL_STEP_UV == 0 &&
        (sram - 500000u) % LGD_RAIL_STEP_UV == 0;
}

/* Floor DDS rather than round upwards: requested OPP is an upper bound.
 * Vendor VCO window 1.5..3.8GHz: /4 for 375..950MHz, /8 below375MHz.
 * Only MFGPLL CON1 is programmable; no FHCTL or CPU PLL protocol is used. */
static inline bool lgd_pll_word(uint32_t khz, uint32_t *word)
{
    uint32_t div, dds;
    if (!word || khz < 299000u || khz > LGD_MAX_KHZ) return false;
    div = khz < 375000u ? 3u : 2u;
    dds = (uint32_t)((uint64_t)khz * 1000u * (1u << div) * 16384u / 26000000u);
    if (!dds || dds > 0x3fffffu) return false;
    *word = LGD_PLL_CHANGE | (div << 24) | dds;
    return true;
}

/* Actual register-derived output, not a frequency-meter measurement. */
static inline bool lgd_pll_hz(uint32_t con1, uint32_t divisor, uint32_t *hz)
{
    uint32_t div = (con1 >> 24) & 7u;
    uint64_t den, rate;
    if (!hz || !divisor || divisor > 6u || div > 4u || !(con1 & 0x3fffffu)) return false;
    den = UINT64_C(16384) * (1u << div) * divisor;
    rate = (UINT64_C(26000000) * (con1 & 0x3fffffu) + den - 1u) / den;
    if (!rate || rate > UINT64_C(1000000000)) return false;
    *hz = (uint32_t)rate; return true;
}

static inline bool lgd_index_from_hz(uint32_t hz, uint32_t *index)
{
    struct lgd_opp opp;
    if (!index) return false;
    for (uint32_t i = 0; i < LGD_OPP_COUNT; i++) {
        (void)lgd_get_opp(i, &opp);
        if (hz <= opp.khz * 1000u && opp.khz * 1000u - hz < 1000u) {
            *index = i; return true;
        }
    }
    return false;
}

/* Smallest canonical OPP rail pair sufficient for a transient source. */
static inline bool lgd_floor_opp(uint32_t hz, struct lgd_opp *opp)
{
    if (!opp || !hz || hz > LGD_MAX_KHZ * 1000u) return false;
    for (uint32_t i = LGD_OPP_COUNT; i-- > 0;) {
        (void)lgd_get_opp(i, opp);
        if (hz <= opp->khz * 1000u) return true;
    }
    return false;
}

/* Software JM occupancy policy, NEVER labelled HW utilization. The adapter
 * supplies real accumulated submission/run wall time (not made-up PMU data).
 * 10..500ms windows, high80%, low30% x3, deadband, at most two OPPs up.
 * Zero-job idle windows select the minimum; a periodic idle tick is required.
 * This selects demand only; lmp_dvfs_target independently enforces live caps. */
struct lgd_governor { uint32_t low_windows; uint64_t last_end_ns; };
struct lgd_load_window {
    uint64_t start_ns, end_ns, busy_ns;
    uint32_t jobs;
    bool valid; /* real monotonic sample, no error/unknown-completion interval */
};
static inline bool lgd_choose(struct lgd_governor *g, uint32_t current,
                             const struct lgd_load_window *w, uint32_t *target)
{
    uint64_t elapsed;
    if (!g || !w || !target || current >= LGD_OPP_COUNT || !w->valid ||
        w->end_ns <= w->start_ns || (g->last_end_ns && w->start_ns != g->last_end_ns)) return false;
    elapsed = w->end_ns - w->start_ns;
    if (elapsed < UINT64_C(10000000) || elapsed > UINT64_C(500000000) ||
        w->busy_ns > elapsed || (!w->jobs && w->busy_ns)) return false;
    g->last_end_ns = w->end_ns; *target = current;
    if (!w->jobs) { g->low_windows = 0; *target = LGD_OPP_COUNT - 1u; }
    else if (w->busy_ns * 100u >= elapsed * 80u) {
        g->low_windows = 0; *target = current > 1u ? current - 2u : 0u;
    } else if (w->busy_ns * 100u <= elapsed * 30u) {
        if (++g->low_windows >= 3u) {
            g->low_windows = 0;
            if (current + 1u < LGD_OPP_COUNT) *target = current + 1u;
        }
    } else g->low_windows = 0;
    return true;
}
#endif
