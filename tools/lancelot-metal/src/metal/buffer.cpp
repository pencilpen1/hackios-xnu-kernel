/* Metal buffers share Mesa's Mali/Panfrost resource and mapping lifetime. */
#include "rwm.h"

#include <stdio.h>
#include <new>

struct rwm_buffer {
	struct rwa_buffer *mesa = nullptr;
	void *contents = nullptr;
	size_t length = 0;

	~rwm_buffer()
	{
		if (mesa != nullptr) {
			rwm_air()->buffer_destroy(mesa);
		}
	}
};

extern "C" struct rwm_buffer *
rwm_buffer_new(struct rwa_device *device, mach_port_t gpu, size_t length, char *why, size_t why_size)
{
	(void)gpu; /* retained in the core ABI; all allocations use the same Mesa device */
	const struct rwm_air *air = rwm_air();
	if (device == nullptr || length == 0 || !rwm_air_available() ||
	    air->buffer_create == nullptr || air->buffer_map == nullptr ||
	    air->buffer_size == nullptr || air->buffer_destroy == nullptr) {
		snprintf(why, why_size, "a buffer of %zu bytes needs the Mali/Panfrost compiler device", length);
		return nullptr;
	}
	auto *buffer = new (std::nothrow) rwm_buffer;
	if (buffer == nullptr) {
		snprintf(why, why_size, "no memory for the buffer's own record");
		return nullptr;
	}
	buffer->mesa = air->buffer_create(device, length, why, why_size);
	if (buffer->mesa == nullptr) {
		delete buffer;
		return nullptr;
	}
	buffer->contents = air->buffer_map(buffer->mesa);
	buffer->length = air->buffer_size(buffer->mesa);
	if (buffer->contents == nullptr || buffer->length < length) {
		snprintf(why, why_size, "Mali/Panfrost allocated %zu bytes but did not map the requested %zu bytes",
			 buffer->length, length);
		delete buffer;
		return nullptr;
	}
	return buffer;
}

extern "C" void rwm_buffer_free(struct rwm_buffer *buffer) { delete buffer; }
extern "C" void *rwm_buffer_contents(const struct rwm_buffer *buffer) { return buffer->contents; }
extern "C" size_t rwm_buffer_length(const struct rwm_buffer *buffer) { return buffer->length; }
extern "C" uint64_t rwm_buffer_gpu_address(const struct rwm_buffer *buffer)
{
	const struct rwm_air *air = rwm_air();
	return air->buffer_gpu_address != nullptr ? air->buffer_gpu_address(buffer->mesa) : 0;
}
extern "C" struct rwa_buffer *rwm_buffer_mesa(const struct rwm_buffer *buffer) { return buffer->mesa; }
