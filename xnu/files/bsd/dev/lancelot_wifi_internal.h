#ifndef LANCELOT_WIFI_INTERNAL_H
#define LANCELOT_WIFI_INTERNAL_H
#include "lancelot_wifi_contract.h"
#include "lancelot_wifi_wire.h"

void lancelot_connsys_init(void);
void lancelot_connsys_quiesce(void);
void lancelot_wifi_start(uint32_t last);
void lancelot_wifi_net_early(void);
void lancelot_wifi_quiesce(void);
void lancelot_wifi_stop(void);
void lancelot_wifi_log(const char *, ...) __attribute__((format(printf, 1, 2)));
uint8_t *lancelot_wifi_file(const char *name, uint32_t *size);
const struct lw_board *lancelot_wifi_board(void);
bool lancelot_wifi_resources(void);
bool lancelot_wifi_stopping(void);
void lancelot_wifi_set_stopping(void);
uintptr_t lancelot_wifi_map(uint64_t physical, uint64_t size, unsigned attributes);
bool lancelot_wifi_pmic_read(uint32_t reg, uint32_t *value);
bool lancelot_wifi_pmic_update(uint32_t reg, uint32_t mask, uint32_t value);
bool lancelot_wifi_pmic_xo_on(void);
bool lancelot_wifi_pmic_xo(bool enabled);
bool lancelot_connsys_return_ready(void);
bool lancelot_wifi_prepare_irq(void);
bool lancelot_wifi_online(void);
bool lancelot_connsys_powered(void);
void lancelot_wifi_revoke(void);

/* Every WLAN/BTIF DMA mapping uses normal uncached memory, including aliases. */
#define LW_MMIO 0x7u
#define LW_UNCACHED 0x6u
#endif
