/* SPDX-License-Identifier: GPL-2.0-only */
#include "lancelot_sspm_pmic.h"

static bool
lsp_address_allowed(uint16_t address)
{
    /* Same pinned MT6358 upmu_hw.h map as lancelot_mali_pmic.c.
     * These are status/selector reads, not measured electrical voltages. */
    switch (address) {
    case 0x000a: /* SWCID, not HWCID */
    case 0x1508: case 0x1520: case 0x1526: case 0x151e:
    case 0x1bc8: case 0x1be4: case 0x1bf4: case 0x1be2:
        return true;
    default:
        return false;
    }
}

static bool
lsp_expired(const struct lsp_io *io, uint64_t start)
{
    uint64_t now = io->now_ns(io->cookie);
    return now < start || now - start >= LSP_TIMEOUT_NS;
}

int
lsp_pmic_read16(struct lsp_pmic *state, uint16_t address, uint16_t *value)
{
    const struct lsp_io *io;
    uint32_t incoming, outgoing, status = 0, raw = 0, i;
    uint64_t start;
    bool modified = false, completed = false;
    int error;
    const uint32_t request[5] = {LSP_READ_REGISTER, address, 0xffffu, 0u, 0u};

    if (!state || !value || !lsp_address_allowed(address))
        return LSP_EINVAL;
    io = &state->io;
    if (!io->acquire || !io->release || !io->poison || !io->read32 ||
        !io->write32 || !io->now_ns || !io->delay_us)
        return LSP_ENOTSUP;
    /* No state/MMIO access until shared-pin ownership and live ABI are proven. */
    error = io->acquire(io->cookie, LSP_TIMEOUT_NS);
    if (error)
        return error;
    if (state->poisoned) {
        error = LSP_EIO;
        goto out;
    }
    if (!state->identity_valid && address != 0x000a) {
        error = LSP_ENOTSUP;
        goto out;
    }
    start = io->now_ns(io->cookie);
    error = io->read32(io->cookie, LSP_CTRL, LSP_IN_IRQ, &incoming);
    if (error)
        goto out;
    error = io->read32(io->cookie, LSP_CTRL, LSP_OUT_IRQ, &outgoing);
    if (error)
        goto out;
    /* Never consume a pre-existing request/ACK. Other pins may be active. */
    if ((incoming | outgoing) & LSP_PMIC_IRQ) {
        error = LSP_EBUSY;
        goto out;
    }
    for (i = 0; i < 5; i++) {
        if (lsp_expired(io, start)) {
            error = LSP_ETIMEDOUT;
            goto out;
        }
        modified = true; /* Even an error may have applied the posted write. */
        error = io->write32(io->cookie, LSP_DATA, LSP_SLOT_OFFSET + 4u * i, request[i]);
        if (error)
            goto out;
    }
    if (lsp_expired(io, start)) {
        error = LSP_ETIMEDOUT;
        goto out;
    }
    error = io->write32(io->cookie, LSP_CTRL, LSP_IN_IRQ, LSP_PMIC_IRQ);
    if (error)
        goto out;
    for (i = 0; i < LSP_MAX_POLLS; i++) {
        if (lsp_expired(io, start)) {
            error = LSP_ETIMEDOUT;
            goto out;
        }
        error = io->read32(io->cookie, LSP_CTRL, LSP_OUT_IRQ, &outgoing);
        if (error)
            goto out;
        if (outgoing & LSP_PMIC_IRQ)
            break;
        io->delay_us(io->cookie, 1);
    }
    if (i == LSP_MAX_POLLS) {
        error = LSP_ETIMEDOUT; /* Also bounded if the clock stalls. */
        goto out;
    }
    if (lsp_expired(io, start)) {
        error = LSP_ETIMEDOUT;
        goto out;
    }
    error = io->read32(io->cookie, LSP_CTRL, LSP_IN_IRQ, &incoming);
    if (error)
        goto out;
    if (incoming & LSP_PMIC_IRQ) {
        error = LSP_EPROTO; /* An ACK while the request is still pending. */
        goto out;
    }
    error = io->read32(io->cookie, LSP_DATA, LSP_SLOT_OFFSET, &status);
    if (error)
        goto out;
    error = io->read32(io->cookie, LSP_DATA, LSP_SLOT_OFFSET + 4u, &raw);
    if (error)
        goto out;
    if (status != 0 || raw > 0xffffu ||
        (address == 0x000a &&
         ((raw != 0x5810u && raw != 0x5820u && raw != 0x5830u) ||
          (state->identity_valid && raw != state->swcid)))) {
        error = LSP_EPROTO;
        goto out;
    }
    if (lsp_expired(io, start)) {
        error = LSP_ETIMEDOUT;
        goto out;
    }
    /* W1C our own bit only. Never clear all of mailbox1's interrupts. */
    error = io->write32(io->cookie, LSP_CTRL, LSP_OUT_IRQ, LSP_PMIC_IRQ);
    if (error)
        goto out;
    error = io->read32(io->cookie, LSP_CTRL, LSP_OUT_IRQ, &outgoing);
    if (error)
        goto out;
    if (outgoing & LSP_PMIC_IRQ) {
        error = LSP_EPROTO;
        goto out;
    }
    if (lsp_expired(io, start)) {
        error = LSP_ETIMEDOUT;
        goto out;
    }
    error = io->read32(io->cookie, LSP_CTRL, LSP_IN_IRQ, &incoming);
    if (error)
        goto out;
    if (incoming & LSP_PMIC_IRQ) {
        error = LSP_EPROTO;
        goto out;
    }
    if (lsp_expired(io, start)) {
        error = LSP_ETIMEDOUT;
        goto out;
    }
    completed = true;
    if (address == 0x000a) {
        state->swcid = (uint16_t)raw;
        state->identity_valid = true;
    }
    *value = (uint16_t)raw; /* Output is unchanged on EVERY failure. */
    error = LSP_OK;
out:
    if (modified && !completed) {
        state->poisoned = true;
        state->identity_valid = false;
        io->poison(io->cookie);
    }
    io->release(io->cookie);
    return error;
}
