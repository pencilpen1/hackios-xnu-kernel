/* Pure RAM fixtures for the real SSPM protocol core, shared by host and XNU.
 * No physical address mapping, IPI, PMIC access, firmware or service publication.
 * RAM responses are synthetic, never hardware identity/rail measurements. */
#ifndef LANCELOT_SSPM_SELFTEST_H
#define LANCELOT_SSPM_SELFTEST_H
#include "lancelot_sspm_pmic.h"

struct lsp_test {
    struct lsp_pmic pmic;
    uint32_t slots[5], in_irq, out_irq, raw, status;
    uint16_t expected_address;
    uint64_t time;
    unsigned reads, writes, releases, poisons;
    bool owned, denied, no_ack, frozen_clock, broken;
};

static int
lsp_test_acquire(void *cookie, uint64_t timeout)
{
    struct lsp_test *t = cookie;
    if (t->owned || timeout != LSP_TIMEOUT_NS) { t->broken = true; return LSP_EBUSY; }
    if (t->denied) return LSP_ENOTSUP;
    if (t->poisons) return LSP_EIO;
    t->owned = true;
    return 0;
}
static void
lsp_test_release(void *cookie)
{
    struct lsp_test *t = cookie;
    if (!t->owned) t->broken = true;
    t->owned = false; t->releases++;
}
static void
lsp_test_poison(void *cookie)
{
    struct lsp_test *t = cookie;
    if (!t->owned || !t->pmic.poisoned) t->broken = true;
    t->poisons++;
}
static int
lsp_test_read(void *cookie, enum lsp_window bank, uint32_t off, uint32_t *value)
{
    struct lsp_test *t = cookie;
    if (!t->owned || t->poisons || !value) { t->broken = true; return LSP_EIO; }
    t->reads++;
    if (bank == LSP_CTRL && (off == LSP_IN_IRQ || off == LSP_OUT_IRQ))
        *value = off == LSP_IN_IRQ ? t->in_irq : t->out_irq;
    else if (bank == LSP_DATA && (off == LSP_SLOT_OFFSET || off == LSP_SLOT_OFFSET + 4))
        *value = t->slots[(off - LSP_SLOT_OFFSET) / 4];
    else { t->broken = true; return LSP_EINVAL; }
    return 0;
}
static int
lsp_test_write(void *cookie, enum lsp_window bank, uint32_t off, uint32_t value)
{
    struct lsp_test *t = cookie;
    if (!t->owned || t->poisons) { t->broken = true; return LSP_EIO; }
    t->writes++;
    if (bank == LSP_DATA && off >= LSP_SLOT_OFFSET && off < LSP_SLOT_OFFSET + 20 && !(off & 3))
        t->slots[(off - LSP_SLOT_OFFSET) / 4] = value;
    else if (bank == LSP_CTRL && value == LSP_PMIC_IRQ && off == LSP_IN_IRQ) {
        if (t->slots[0] != LSP_READ_REGISTER || t->slots[1] != t->expected_address ||
            t->slots[2] != 0xffff || t->slots[3] || t->slots[4]) {
            t->broken = true; return LSP_EPROTO;
        }
        t->in_irq |= value;
        if (!t->no_ack) {
            t->in_irq &= ~value;
            t->slots[0] = t->status; t->slots[1] = t->raw;
            t->out_irq |= value;
        }
    } else if (bank == LSP_CTRL && value == LSP_PMIC_IRQ && off == LSP_OUT_IRQ)
        t->out_irq &= ~value;
    else { t->broken = true; return LSP_EINVAL; }
    return 0;
}
static uint64_t lsp_test_now(void *cookie) { return ((struct lsp_test *)cookie)->time; }
static void
lsp_test_delay(void *cookie, uint32_t us)
{
    struct lsp_test *t = cookie;
    if (us != 1) t->broken = true;
    if (!t->frozen_clock) t->time += 2000;
}
static void
lsp_test_init(struct lsp_test *t)
{
    *t = (struct lsp_test){0};
    t->raw = 0x5830;
    t->expected_address = 0x000a;
    t->pmic.io = (struct lsp_io){t, lsp_test_acquire, lsp_test_release, lsp_test_poison,
        lsp_test_read, lsp_test_write, lsp_test_now, lsp_test_delay};
}

/* Returns failing line, never fabricates a success or hardware observation. */
static unsigned
lancelot_sspm_protocol_selftest(void)
{
    struct lsp_test t;
    uint16_t value = 0xdead;
    unsigned reads, writes;
#define LSP_CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)
    lsp_test_init(&t); t.denied = true;
    LSP_CHECK(lsp_pmic_read16(&t.pmic, 0x000a, &value) == LSP_ENOTSUP);
    LSP_CHECK(!t.reads && !t.writes && !t.releases && value == 0xdead && !t.broken);
    lsp_test_init(&t);
    LSP_CHECK(lsp_pmic_read16(&t.pmic, 0x1508, &value) == LSP_ENOTSUP);
    LSP_CHECK(!t.reads && !t.writes && t.releases == 1 && !t.broken);
    LSP_CHECK(lsp_pmic_read16(&t.pmic, 0x1509, &value) == LSP_EINVAL);
    for (unsigned which = 0; which < 2; which++) {
        lsp_test_init(&t);
        if (which) t.in_irq = LSP_PMIC_IRQ; else t.out_irq = LSP_PMIC_IRQ;
        LSP_CHECK(lsp_pmic_read16(&t.pmic, 0x000a, &value) == LSP_EBUSY);
        LSP_CHECK(!t.writes && !t.poisons && value == 0xdead && !t.broken);
    }
    lsp_test_init(&t); t.in_irq = 1u << 5; t.out_irq = 1u << 6;
    LSP_CHECK(!lsp_pmic_read16(&t.pmic, 0x000a, &value));
    LSP_CHECK(value == 0x5830 && t.pmic.identity_valid && t.pmic.swcid == value && t.writes == 7);
    LSP_CHECK(t.in_irq == (1u << 5) && t.out_irq == (1u << 6) && !t.broken);
    const uint16_t rails[] = {0x1508, 0x1520, 0x1526, 0x151e,
                             0x1bc8, 0x1be4, 0x1bf4, 0x1be2};
    for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
        t.expected_address = rails[i];
        t.raw = 0x1234u + i;
        LSP_CHECK(!lsp_pmic_read16(&t.pmic, rails[i], &value) && value == t.raw);
        LSP_CHECK(t.in_irq == (1u << 5) && t.out_irq == (1u << 6) && !t.broken);
    }
    t.expected_address = 0x000a;
    t.raw = 0x5820; value = 0xdead; /* A chip revision cannot change mid-boot. */
    LSP_CHECK(lsp_pmic_read16(&t.pmic, 0x000a, &value) == LSP_EPROTO);
    LSP_CHECK(t.poisons == 1 && t.pmic.poisoned && !t.pmic.identity_valid && !t.broken);
    LSP_CHECK(value == 0xdead);
    for (unsigned which = 0; which < 5; which++) {
        lsp_test_init(&t); value = 0xdead;
        if (which == 0) t.raw = 0x5831; /* Wrong HWCID instead of SWCID. */
        if (which == 1) t.status = 1;
        if (which == 2) t.raw = 0x10000;
        if (which >= 3) t.no_ack = true;
        if (which == 4) t.frozen_clock = true;
        LSP_CHECK(lsp_pmic_read16(&t.pmic, 0x000a, &value) ==
            (which >= 3 ? LSP_ETIMEDOUT : LSP_EPROTO));
        LSP_CHECK(t.poisons == 1 && t.releases == 1 && t.pmic.poisoned && !t.owned && !t.broken);
        LSP_CHECK(value == 0xdead && t.reads <= LSP_MAX_POLLS + 2);
        reads = t.reads; writes = t.writes;
        LSP_CHECK(lsp_pmic_read16(&t.pmic, 0x000a, &value) == LSP_EIO);
        LSP_CHECK(t.reads == reads && t.writes == writes && t.releases == 1 && value == 0xdead);
    }
    return 0;
#undef LSP_CHECK
}
#endif
