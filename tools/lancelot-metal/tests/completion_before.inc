/* Original MTK completion function, retained as a failure-injection control. */
static int
completion_before(struct rwm_command *c, struct rwa_device *device, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	if (c->queued && !c->processed && !rwm_worker_on()) {
		rwm_command_wait_processed(c);
	}
	if (c->status == RWM_CB_ERROR || c->status == RWM_CB_COMPLETED) {
		return 0;
	}
	if (c->seq != 0) {
		const uint64_t from = rwm_ns();
		if (air->wait_seq(c->seq, 5000000000ll) != 0) {
			rwm_print("THE GPU DID NOT FINISH job %llu\n", (unsigned long long)c->seq);
		}
		rwm_perf.completed_waits++;
		rwm_perf.completed_ns += rwm_ns() - from;
		c->status = RWM_CB_COMPLETED;
		return 0;
	}
	if (air->finish(device, why, why_size) != 0) {
		c->status = RWM_CB_ERROR;
		return -1;
	}
	c->status = RWM_CB_COMPLETED;
	return 0;
}
