/* Host fixtures ONLY. No phone/backend/firmware MMIO is executed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lancelot_sspm_pmic.h"

struct fake {
    struct lsp_pmic state;
    uint32_t data[32], incoming, outgoing, status, raw;
    uint16_t expected_address;
    uint64_t time;
    unsigned ops, reads, writes, acquisitions, releases, poisons, fail_op;
    unsigned time_jump_op;
    uint64_t time_jump_value;
    bool owned, blocked, deny, hang, frozen_clock, sticky_ack, keep_request;
    bool request_after_ack;
};

static int acquire(void *cookie, uint64_t timeout)
{
    struct fake *f = cookie;
    assert(timeout == LSP_TIMEOUT_NS && !f->owned);
    f->acquisitions++;
    if (f->blocked) return LSP_EIO;
    if (f->deny) return LSP_ENOTSUP;
    f->owned = true;
    return 0;
}

static void release(void *cookie)
{
    struct fake *f = cookie;
    assert(f->owned);
    f->owned = false;
    f->releases++;
}

static void poison(void *cookie)
{
    struct fake *f = cookie;
    assert(f->owned && f->state.poisoned);
    f->blocked = true;
    f->poisons++;
}

static int read32(void *cookie, enum lsp_window window, uint32_t offset, uint32_t *value)
{
    struct fake *f = cookie;
    assert(f->owned && !f->blocked && !(offset & 3));
    f->ops++; f->reads++;
    if (f->ops == f->time_jump_op) f->time = f->time_jump_value;
    if (window == LSP_DATA) {
        assert(offset == LSP_SLOT_OFFSET || offset == LSP_SLOT_OFFSET + 4);
        *value = f->data[offset / 4];
    } else {
        assert(window == LSP_CTRL && (offset == LSP_IN_IRQ || offset == LSP_OUT_IRQ));
        *value = offset == LSP_IN_IRQ ? f->incoming : f->outgoing;
    }
    return f->ops == f->fail_op ? LSP_EIO : 0;
}

static int write32(void *cookie, enum lsp_window window, uint32_t offset, uint32_t value)
{
    struct fake *f = cookie;
    assert(f->owned && !f->blocked && !(offset & 3));
    f->ops++; f->writes++;
    if (f->ops == f->time_jump_op) f->time = f->time_jump_value;
    if (window == LSP_DATA) {
        assert(offset >= LSP_SLOT_OFFSET && offset < LSP_SLOT_OFFSET + 20);
        f->data[offset / 4] = value;
    } else {
        assert(window == LSP_CTRL && value == LSP_PMIC_IRQ);
        if (offset == LSP_IN_IRQ) {
            assert(f->data[10] == 0x202 && f->data[12] == 0xffff);
            assert(f->data[11] == f->expected_address);
            assert(f->data[13] == 0 && f->data[14] == 0);
            f->incoming |= value;
            if (!f->hang) {
                if (!f->keep_request) f->incoming &= ~value;
                f->data[10] = f->status;
                f->data[11] = f->raw;
                f->outgoing |= value;
            }
        } else {
            assert(offset == LSP_OUT_IRQ);
            if (!f->sticky_ack) f->outgoing &= ~value;
            if (f->request_after_ack) f->incoming |= LSP_PMIC_IRQ;
        }
    }
    /* Failure AFTER applying a posted write: still must quarantine. */
    return f->ops == f->fail_op ? LSP_EIO : 0;
}

static uint64_t now_ns(void *cookie) { return ((struct fake *)cookie)->time; }
static void delay_us(void *cookie, uint32_t usec)
{
    struct fake *f = cookie;
    assert(usec == 1);
    if (!f->frozen_clock) f->time += 2000;
}

static void init(struct fake *f)
{
    memset(f, 0, sizeof(*f));
    f->raw = 0x5830;
    f->expected_address = 0x000a;
    f->state.io = (struct lsp_io){f, acquire, release, poison, read32, write32, now_ns, delay_us};
}

static void check_poison(struct fake *f, uint16_t address, int expected_error)
{
    uint16_t output = 0xdead;
    unsigned operations;
    f->expected_address = address;
    assert(lsp_pmic_read16(&f->state, address, &output) == expected_error);
    assert(output == 0xdead && f->poisons == 1 && f->releases == 1 && !f->owned);
    assert(f->state.poisoned && !f->state.identity_valid);
    operations = f->ops;
    assert(lsp_pmic_read16(&f->state, 0x000a, &output) == LSP_EIO);
    assert(f->ops == operations && output == 0xdead && f->releases == 1);
}

int main(void)
{
    struct fake f;
    uint16_t output = 0xdead;
    unsigned successful_ops;
    assert(LSP_PMIC_IRQ == 4 && LSP_SLOT_OFFSET == 40);
    assert(lsp_pmic_read16(NULL, 0x000a, &output) == LSP_EINVAL);
    init(&f);
    f.state.io.acquire = NULL;
    assert(lsp_pmic_read16(&f.state, 0x000a, &output) == LSP_ENOTSUP && f.ops == 0);
    init(&f); f.deny = true;
    assert(lsp_pmic_read16(&f.state, 0x000a, &output) == LSP_ENOTSUP);
    assert(f.ops == 0 && f.releases == 0);
    init(&f);
    assert(lsp_pmic_read16(&f.state, 0x1508, &output) == LSP_ENOTSUP && f.ops == 0);
    assert(f.releases == 1);
    for (unsigned address = 0; address <= 0xffff; address++) {
        switch (address) {
        case 0x000a: case 0x1508: case 0x1520: case 0x1526: case 0x151e:
        case 0x1bc8: case 0x1be4: case 0x1bf4: case 0x1be2: continue;
        }
        init(&f);
        assert(lsp_pmic_read16(&f.state, (uint16_t)address, &output) == LSP_EINVAL);
        assert(f.ops == 0 && f.acquisitions == 0);
    }
    for (unsigned which = 0; which < 2; which++) {
        init(&f);
        if (which) f.outgoing = LSP_PMIC_IRQ; else f.incoming = LSP_PMIC_IRQ;
        assert(lsp_pmic_read16(&f.state, 0x000a, &output) == LSP_EBUSY);
        assert(f.writes == 0 && f.poisons == 0 && output == 0xdead);
    }
    init(&f); f.incoming = 1u << 5; f.outgoing = 1u << 6;
    assert(lsp_pmic_read16(&f.state, 0x000a, &output) == 0);
    assert(output == 0x5830 && f.state.swcid == output && f.state.identity_valid);
    assert(f.writes == 7 && f.incoming == (1u << 5) && f.outgoing == (1u << 6));
    successful_ops = f.ops;
    const uint16_t rails[] = {0x1508, 0x1520, 0x1526, 0x151e,
                             0x1bc8, 0x1be4, 0x1bf4, 0x1be2};
    for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
        f.expected_address = rails[i];
        f.raw = 0x1234u + i;
        assert(lsp_pmic_read16(&f.state, rails[i], &output) == 0 && output == f.raw);
        assert(f.incoming == (1u << 5) && f.outgoing == (1u << 6));
    }
    for (unsigned cid = 0x5810; cid <= 0x5830; cid += 0x10) {
        init(&f); f.raw = cid;
        assert(lsp_pmic_read16(&f.state, 0x000a, &output) == 0 && output == cid);
    }
    /* Every transport read/write position, including the final ACK readback. */
    for (unsigned operation = 1; operation <= successful_ops; operation++) {
        init(&f); f.fail_op = operation; output = 0xdead;
        if (operation <= 2) {
            assert(lsp_pmic_read16(&f.state, 0x000a, &output) == LSP_EIO);
            assert(f.writes == 0 && f.poisons == 0 && f.releases == 1 && output == 0xdead);
        } else check_poison(&f, 0x000a, LSP_EIO);
    }
    init(&f); f.hang = true; check_poison(&f, 0x000a, LSP_ETIMEDOUT);
    assert(f.reads < LSP_MAX_POLLS + 3);
    init(&f); f.hang = true; f.frozen_clock = true; check_poison(&f, 0x000a, LSP_ETIMEDOUT);
    assert(f.reads == LSP_MAX_POLLS + 2);
    init(&f); f.status = 1; check_poison(&f, 0x000a, LSP_EPROTO);
    init(&f); f.status = 0xffffffffu; check_poison(&f, 0x000a, LSP_EPROTO);
    init(&f); f.raw = 0x10000; check_poison(&f, 0x000a, LSP_EPROTO);
    init(&f); f.raw = 0x5831; check_poison(&f, 0x000a, LSP_EPROTO); /* HWCID is not SWCID. */
    init(&f); f.sticky_ack = true; check_poison(&f, 0x000a, LSP_EPROTO);
    init(&f); f.keep_request = true; check_poison(&f, 0x000a, LSP_EPROTO);
    init(&f); f.request_after_ack = true; check_poison(&f, 0x000a, LSP_EPROTO);
    init(&f);
    assert(!lsp_pmic_read16(&f.state, 0x000a, &output));
    f.raw = 0x5820; f.releases = 0;
    check_poison(&f, 0x000a, LSP_EPROTO); /* Valid but changed identity must not be accepted. */
    /* Expiration before any mailbox write is non-poisoning. Deadline/rollback
     * after any possibly posted payload write must fail before publishing data. */
    init(&f); output = 0xdead; f.time_jump_op = 2; f.time_jump_value = LSP_TIMEOUT_NS;
    assert(lsp_pmic_read16(&f.state, 0x000a, &output) == LSP_ETIMEDOUT);
    assert(f.writes == 0 && f.poisons == 0 && output == 0xdead);
    for (unsigned operation = 3; operation <= successful_ops; operation++) {
        init(&f); f.time_jump_op = operation; f.time_jump_value = LSP_TIMEOUT_NS;
        check_poison(&f, 0x000a, LSP_ETIMEDOUT);
    }
    for (unsigned operation = 3; operation <= successful_ops; operation++) {
        init(&f); f.time = 100; f.time_jump_op = operation; f.time_jump_value = 99;
        check_poison(&f, 0x000a, LSP_ETIMEDOUT);
    }
    printf("SSPM production core: 65527 rejected addresses, 8 rail wire addresses, %u fault positions, exact timeout/rollback errors, foreign/other IRQ, identity, ACK, poison/retry PASS; NO HARDWARE\n", successful_ops);
    return 0;
}
