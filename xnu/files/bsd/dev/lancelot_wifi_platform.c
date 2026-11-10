/* Lancelot resource ownership shared by CONNSYS and the native gen4m station.
 * Register order comes from the MT6768 driver; all WACS2 transactions use the
 * same arbiter as Mali. A failed in-flight command quarantines that channel.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include "lancelot_wifi_internal.h"
#include "lancelot_pwrap.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void gic_v3_spi_mask(uint32_t);
static LCK_GRP_DECLARE(lw_platform_group, "lancelot-wifi-platform");
static LCK_MTX_DECLARE(lw_map_lock, &lw_platform_group);
static struct lw_handoff lw_offer;
static bool lw_resources_checked, lw_resources_ok;
static bool lw_stopping;

static struct {
    uint64_t pa, size;
    unsigned attributes;
    vm_offset_t va;
} lw_regions[] = {
    { 0x10000000, 0x10000, LW_MMIO, 0 },
    { 0x11000000, 0x10000, LW_MMIO, 0 },
    { 0x18000000, 0x100000, LW_MMIO, 0 },
    { 0x0c538000, 0x4000, LW_MMIO, 0 }, /* MT6768 SYSIRQ polarity */
    { LW_EMI_PHYS, LW_EMI_SIZE, LW_UNCACHED, 0 },
    { LW_DMA_PHYS, LW_DMA_SIZE, LW_UNCACHED, 0 },
};

void
lancelot_wifi_log(const char *format, ...)
{
    char line[768];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    redwood_wifi_tell("%s\n", line);
}

bool
lancelot_wifi_stopping(void)
{
    return __atomic_load_n(&lw_stopping, __ATOMIC_ACQUIRE);
}

void
lancelot_wifi_set_stopping(void)
{
    __atomic_store_n(&lw_stopping, true, __ATOMIC_RELEASE);
}

const struct lw_board *
lancelot_wifi_board(void)
{
    return lw_resources_ok ? &lw_offer.board : NULL;
}

bool
lancelot_wifi_resources(void)
{
    DTEntry chosen;
    const void *value;
    unsigned size;
    uint64_t dram = 0, length = 0;
    if (lw_resources_checked) return lw_resources_ok;
    lw_resources_checked = true;
    /* The established board discriminator is the MT6768 peripheral base. */
    if (!lancelot_touch_platform() ||
        SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "dram-base", &value, &size) != kSuccess || size != 8)
        return false;
    memcpy(&dram, value, sizeof(dram));
    if (SecureDTGetProperty(chosen, "dram-size", &value, &size) != kSuccess || size != 8)
        return false;
    memcpy(&length, value, sizeof(length));
    if (SecureDTGetProperty(chosen, "lancelot-wifi", &value, &size) != kSuccess || size != sizeof(lw_offer))
        return false;
    memcpy(&lw_offer, value, sizeof(lw_offer));
    if (!lw_handoff_valid(&lw_offer, dram, length)) return false;
    if (lancelot_pwrap_ensure_board_handoff()) return false;
    lw_resources_ok = true;
    /* Materialize all windows before any driver dereferences them. Mapping a
     * powered-off register bank does not read it. */
    for (unsigned i = 0; i < sizeof(lw_regions) / sizeof(lw_regions[0]); i++) {
        if (!lancelot_wifi_map(lw_regions[i].pa, lw_regions[i].size, lw_regions[i].attributes)) {
            lw_resources_ok = false;
            return false;
        }
    }
    for (unsigned spi = 0; spi < 5; spi++) {
        static const uint32_t interrupts[] = { 133, 115, 141, 284, 285 };
        gic_v3_spi_mask(32 + interrupts[spi]);
    }
    lancelot_wifi_log("lancelot-wifi: loader reservation accepted EMI=%#llx/%#llx DMA=%#llx/%#llx",
        lw_offer.emi_phys, lw_offer.emi_size, lw_offer.dma_phys, lw_offer.dma_size);
    return true;
}

uintptr_t
lancelot_wifi_map(uint64_t physical, uint64_t size, unsigned requested)
{
    uintptr_t result = 0;
    (void)requested;
    if (!size || physical > UINT64_MAX - size || !lancelot_touch_platform())
        return 0;
    lck_mtx_lock(&lw_map_lock);
    for (unsigned i = 0; i < sizeof(lw_regions) / sizeof(lw_regions[0]); i++) {
        uint64_t start = lw_regions[i].pa;
        if (physical < start || physical - start > lw_regions[i].size ||
            size > lw_regions[i].size - (physical - start)) continue;
        if (lw_regions[i].attributes == LW_UNCACHED && !lw_resources_ok) break;
        if (!lw_regions[i].va)
            lw_regions[i].va = ml_io_map_unmappable(start, lw_regions[i].size, lw_regions[i].attributes);
        if (lw_regions[i].va) result = lw_regions[i].va + (physical - start);
        break;
    }
    lck_mtx_unlock(&lw_map_lock);
    return result;
}

bool
lancelot_wifi_prepare_irq(void)
{
    if (!lw_resources_ok || lancelot_wifi_stopping()) return false;
    uintptr_t polarity = lancelot_wifi_map(0x0c53a670, 4, LW_MMIO);
    if (!polarity) return false;
    /* SYSIRQ index is the DT SPI (285); GIC's INTID is 285 + 32. */
    *(volatile uint32_t *)polarity |= 1u << 29;
    __asm__ volatile("dsb sy" ::: "memory");
    return (*(volatile uint32_t *)polarity & (1u << 29)) != 0;
}

static uint64_t
lw_ns(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

static uint32_t
lw_read(uintptr_t base, unsigned offset)
{
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t value = *(volatile uint32_t *)(base + offset);
    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}

static void
lw_write(uintptr_t base, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Called with the shared AP WACS2 lock held. No unowned response is cleared. */
static bool
lw_pmic_wait(uintptr_t base, unsigned fsm, bool owned, uint32_t *out)
{
    uint64_t began = lw_ns();
    for (unsigned polls = 0; polls < 10000; polls++) {
        uint32_t raw = lw_read(base, 0xc24);
        if (!(raw & (1u << 21))) break; /* INIT_DONE */
        if (((raw >> 16) & 7) == fsm && (raw & (1u << 20)) && (fsm || !(raw & (1u << 19)))) {
            if (out) *out = raw;
            return true;
        }
        if (lw_ns() - began >= UINT64_C(10000000)) break;
        delay(1);
    }
    if (owned) (void)lancelot_pwrap_poison();
    return false;
}

static bool
lw_pmic_read_locked(uintptr_t base, uint16_t reg, uint32_t *value)
{
    uint32_t raw;
    if (!lw_pmic_wait(base, 0, false, NULL)) return false;
    lw_write(base, 0xc20, (uint32_t)(reg >> 1) << 16);
    if (!lw_pmic_wait(base, 6, true, &raw)) return false;
    lw_write(base, 0xc28, 1);
    if (!lw_pmic_wait(base, 0, true, NULL)) return false;
    *value = raw & 0xffff;
    return true;
}

static bool
lw_pmic_write_locked(uintptr_t base, uint16_t reg, uint32_t value)
{
    if (!lw_pmic_wait(base, 0, false, NULL)) return false;
    lw_write(base, 0xc20, (1u << 31) | ((uint32_t)(reg >> 1) << 16) | (value & 0xffff));
    return lw_pmic_wait(base, 0, true, NULL);
}

static bool
lw_pmic_allowed(uint32_t reg)
{
    switch (reg) {
    case 0x000a: case 0x0788: case 0x078a: case 0x078e:
    case 0x1c58: case 0x1c5a: case 0x1e8c: /* VCN18 */
    case 0x1d88: case 0x1d8a: case 0x1d90: case 0x1e14: /* VCN28 */
    case 0x1d1c: case 0x1d1e: case 0x1d2a: case 0x1e28: /* VCN33 */
        return true;
    default: return false;
    }
}

bool
lancelot_wifi_pmic_read(uint32_t reg, uint32_t *value)
{
    if (!value || !lw_pmic_allowed(reg) || lancelot_wifi_stopping() || lancelot_pwrap_ensure_board_handoff()) return false;
    uintptr_t base = lancelot_wifi_map(0x1000d000, 0x1000, LW_MMIO);
    if (!base || lancelot_pwrap_acquire(NULL, UINT64_C(10000000))) return false;
    bool ok = lw_pmic_read_locked(base, (uint16_t)reg, value);
    lancelot_pwrap_release(NULL);
    return ok;
}

bool
lancelot_wifi_pmic_update(uint32_t reg, uint32_t mask, uint32_t value)
{
    if (!lw_pmic_allowed(reg) || reg == 0x000a || (mask & ~0xffffu) ||
        !lw_resources_ok || lancelot_wifi_stopping()) return false;
    if (reg == 0x788 && mask == 0x20) return lancelot_wifi_pmic_xo((value & 0x20) != 0);
    uintptr_t base = lancelot_wifi_map(0x1000d000, 0x1000, LW_MMIO);
    if (!base || lancelot_pwrap_acquire(NULL, UINT64_C(10000000))) return false;
    uint32_t before = 0, after = 0;
    bool ok = lw_pmic_read_locked(base, (uint16_t)reg, &before) &&
        lw_pmic_write_locked(base, (uint16_t)reg, (before & ~mask) | (value & mask)) &&
        lw_pmic_read_locked(base, (uint16_t)reg, &after) && (after & mask) == (value & mask);
    lancelot_pwrap_release(NULL);
    return ok;
}

bool
lancelot_wifi_pmic_xo(bool enabled)
{
    if (!lw_resources_ok || lancelot_wifi_stopping()) return false;
    uintptr_t base = lancelot_wifi_map(0x1000d000, 0x1000, LW_MMIO);
    if (!base || lancelot_pwrap_acquire(NULL, UINT64_C(10000000))) return false;
    uint32_t after = 0;
    /* The SET alias preserves the other clock buffers, including hardware
     * controlled bits, even if another PMIC requester changes those bits. */
    bool ok = lw_pmic_write_locked(base, enabled ? 0x078a : 0x078c, 0x20) &&
        lw_pmic_read_locked(base, 0x0788, &after) && ((after & 0x20) != 0) == enabled;
    lancelot_pwrap_release(NULL);
    return ok;
}

bool lancelot_wifi_pmic_xo_on(void) { return lancelot_wifi_pmic_xo(true); }
