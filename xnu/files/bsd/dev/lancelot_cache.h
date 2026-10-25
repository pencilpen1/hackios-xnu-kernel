/* ARMv8 architected cache geometry; no MMIO, cache maintenance or fake DT. */
#ifndef LANCELOT_CACHE_H
#define LANCELOT_CACHE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
struct lancelot_cache_geometry {
	uint32_t line, ways, sets;
	uint64_t bytes;
};
static inline bool
lancelot_cache_geometry(uint64_t ccsidr, unsigned ccidx, struct lancelot_cache_geometry *out)
{
	if (!out || ccidx > 1) return false;
	struct lancelot_cache_geometry g = {0};
	g.line = 1u << ((ccsidr & 7) + 4);
	if (ccidx == 0) {
		g.ways = (uint32_t)((ccsidr >> 3) & 0x3ff) + 1;
		g.sets = (uint32_t)((ccsidr >> 13) & 0x7fff) + 1;
	} else {
		g.ways = (uint32_t)((ccsidr >> 3) & 0x1fffff) + 1;
		g.sets = (uint32_t)((ccsidr >> 32) & 0xffffff) + 1;
	}
	g.bytes = (uint64_t)g.line * g.ways * g.sets;
	*out = g;
	return true;
}

/* Private bounded working-set fixture: one next-index per cache line,
 * shuffled into a complete cycle. Corrupt indices are rejected before use. */
static inline bool
lancelot_cache_ring(void *memory, uint32_t *order, uint32_t bytes, uint32_t line, uint32_t seed)
{
	if (!memory || !order || ((uintptr_t)memory & 7) || line < 8 || line > 2048 || (line & (line - 1)) ||
	    !bytes || bytes > (4u << 20) || bytes % line) return false;
	uint32_t nodes = bytes / line;
	for (uint32_t i = 0; i < nodes; i++) order[i] = i;
	if (!seed) seed = 1;
	for (uint32_t i = nodes - 1; i; i--) {
		seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
		uint32_t at = seed % (i + 1), old = order[i]; order[i] = order[at]; order[at] = old;
	}
	for (uint32_t i = 0; i < nodes; i++)
		*(uint32_t *)((uint8_t *)memory + (size_t)order[i] * line) = order[(i + 1) % nodes];
	return true;
}
static inline bool
lancelot_cache_chase(const void *memory, uint32_t line, uint32_t nodes, uint32_t loops, uint32_t *index)
{
	if (!memory || !index || ((uintptr_t)memory & 3) || line < 8 || line > 2048 || (line & (line - 1)) || !nodes ||
	    nodes > (4u << 20) / line || *index >= nodes || loops > (1u << 20)) return false;
	uint32_t at = *index;
	for (uint32_t i = 0; i < loops; i++) {
		at = *(const volatile uint32_t *)((const uint8_t *)memory + (size_t)at * line);
		if (at >= nodes) return false;
	}
	*index = at;
	return true;
}
#endif
