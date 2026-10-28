/* SPDX-License-Identifier: MIT */
#ifndef LANCELOT_TRANSPORT_H
#define LANCELOT_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
/* Host tests replace these five functions; production uses IOKit directly. */
int lm_transport_open(uintptr_t *connection);
void lm_transport_close(uintptr_t connection);
int lm_transport_call(uintptr_t connection, uint32_t selector, void *data, size_t size);
int lm_transport_map(uintptr_t connection, uint32_t handle, uintptr_t *address, uint64_t *size);
int lm_transport_unmap(uintptr_t connection, uint32_t handle, uintptr_t address);
#endif
