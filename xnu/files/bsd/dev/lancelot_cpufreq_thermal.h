/* Optional MT6768 CPU temperature provider, independent of SSPM admission.
 * A failed start must release only its own thermal resources and must not
 * alter CPU-DVFS state. A running provider's failed read is NOT absence. */
#ifndef LANCELOT_CPUFREQ_THERMAL_H
#define LANCELOT_CPUFREQ_THERMAL_H
#include <stdbool.h>
#include <stdint.h>

#define LCF_THERMAL_START_NS UINT64_C(100000000)
#define LCF_THERMAL_AGE_NS UINT64_C(250000000)
#define LCF_THERMAL_BOOST_MAX_MC 70000

struct lcf_thermal_reading {
    uint64_t at_ns; /* Mach absolute time converted to ns, not CLOCK_MONOTONIC. */
    int32_t max_mC; /* Maximum of the actual CPU sensors, not battery/PMIC temp. */
    bool calibrated;
};

/* start must honor this absolute deadline and return an error if calibration,
 * resource ownership or sampling cannot be established. stop is idempotent,
 * bounded and safe even after a partially failed start. Neither callback may
 * reset/reconfigure SSPM, PWRAP, CPU clocks or another driver's resources. */
int lancelot_cpu_thermal_start(uint64_t deadline_ns);
int lancelot_cpu_thermal_read(struct lcf_thermal_reading *reading);
void lancelot_cpu_thermal_stop(void);

enum lcf_thermal_policy {
    LCF_THERMAL_OPTIONAL_OFF,
    LCF_THERMAL_LIVE,
    LCF_THERMAL_LIMIT
};

static inline enum lcf_thermal_policy
lcf_thermal_classify(bool started, int read_error,
    const struct lcf_thermal_reading *reading, uint64_t now_ns)
{
    if (!started) return LCF_THERMAL_OPTIONAL_OFF;
    if (read_error || !reading || !reading->calibrated || !reading->at_ns ||
        now_ns < reading->at_ns || now_ns - reading->at_ns > LCF_THERMAL_AGE_NS ||
        reading->max_mC < -40000 || reading->max_mC > LCF_THERMAL_BOOST_MAX_MC)
        return LCF_THERMAL_LIMIT;
    return LCF_THERMAL_LIVE;
}
#endif
