/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LANCELOT_SSPM_PMIC_H
#define LANCELOT_SSPM_PMIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MT6768 SSPM v1: pin 8, mailbox1 start pin 6 => IRQ bit 2, NOT bit 8.
 * The DT reserves 0x100 bytes; this ABI uses 32 four-byte slots (0x80).
 * See docs/LANCELOT-SSPM-PMIC-2026-10-25.md for pinned source and limitations. */
#define LSP_DATA_PHYS UINT64_C(0x10460000)
#define LSP_CTRL_PHYS UINT64_C(0x10461000)
#define LSP_SLOT_OFFSET 40u
#define LSP_IN_IRQ 0u
#define LSP_OUT_IRQ 4u
#define LSP_PMIC_IRQ (1u << 2)
#define LSP_READ_REGISTER 0x202u
#define LSP_TIMEOUT_NS UINT64_C(10000000)
#define LSP_MAX_POLLS 10000u

enum lsp_window { LSP_DATA, LSP_CTRL };
enum lsp_error {
    LSP_OK = 0, LSP_EIO = -5, LSP_EBUSY = -16, LSP_EINVAL = -22,
    LSP_EPROTO = -71, LSP_ENOTSUP = -95, LSP_ETIMEDOUT = -110
};

struct lsp_io {
    void *cookie;
    /* Must own the PMIC pin, shared slot and its IRQ consumer for the entire
     * transaction, and prove the live firmware ABI/permissions after handoff.
     * Reject without any MMIO if not proven. Neither idle nor Android 'ready'
     * proves this. No implementation is currently registered by XNU.
     * acquire also serializes all access to this state (including identity).
     * Same firmware is the intended service peer, NOT a peer to suspend.
     */
    int (*acquire)(void *cookie, uint64_t timeout_ns);
    void (*release)(void *cookie);
    /* Persistent shared-pin quarantine BEFORE release. On uncertain completion
     * retain any firmware exclusion token; block all subsequent pin users.
     * Do not cancel/reset SSPM or clear a foreign/late ACK to recover. */
    void (*poison)(void *cookie);
    /* Backend provides ordered device-memory access; slot writes precede IN_IRQ,
     * ACK observation precedes reply reads, reply reads precede OUT_IRQ W1C.
     * Backend calls themselves must be bounded. */
    int (*read32)(void *cookie, enum lsp_window window, uint32_t offset, uint32_t *value);
    int (*write32)(void *cookie, enum lsp_window window, uint32_t offset, uint32_t value);
    uint64_t (*now_ns)(void *cookie);
    void (*delay_us)(void *cookie, uint32_t usec);
};

/* Zero initialize once, assign io, then share one state per PMIC pin.
 * No runtime reset/reprobe API: a poison cannot be cleared by retrying init.
 * All mutable fields are accessed under io.acquire. */
struct lsp_pmic {
    struct lsp_io io;
    bool poisoned;
    bool identity_valid;
    uint16_t swcid;
};

/* Only SWCID and the eight source-derived GPU rail status/selector registers.
 * First successful read MUST be SWCID (0x000a), with E1/E2/E3 software ID.
 * No PMIC write opcode or voltage/enable operation is exposed.
 * Transport does write mailbox payload/doorbell/own ACK; this is NOT passive
 * collection. Fixed two-word ACK has NO sequence echo or length field. */
int lsp_pmic_read16(struct lsp_pmic *state, uint16_t address, uint16_t *value);

#ifdef __cplusplus
}
#endif
#endif
