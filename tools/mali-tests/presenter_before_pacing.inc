/* W7 production show() prefix: read ownership spans the refresh delay. */

	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint64_t entered=began, gap_us=0;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;
	LancelotMaliReadGroup readers;
	LancelotMaliQueueReadGroup queueReaders;
	bool gpuReady = true;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && (!MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS) ||
		    !MiniIOSurfaceMaliDrawn(surfaces[layer], SWAP_GPU_MS))) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; dropping swap\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
			gpuReady = false;
		}
	}
	/* Same root order as GPU lease acquisition. Hold reader ownership through
	 * ALL draw/helper/record work; a later writer cannot enter after waitCpu. */
	for (unsigned layer = 0; gpuReady && layer < SWAP_LAYERS; layer++) {
		if (!queueReaders.add(surfaces[layer], (uint64_t)SWAP_GPU_MS * 1000000)) {
			gpuReady = false;
			break;
		}
		LancelotMaliCpuLease *lease = surfaces[layer] ? surfaces[layer]->maliCpuLease : NULL;
		if (!readers.add(lease)) gpuReady = false;
	}
	if (gpuReady && readers.acquire((uint64_t)SWAP_GPU_MS * 1000000) != LMAL_OK)
		gpuReady = false;
	fStatWaiting += mach_absolute_time() - began;
	if (!gpuReady) {
		readers.releaseReaders();
		IOLockLock(fSwapLock);
		fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
		fSwapsWaiting--;
		fStatLost++;
		IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
		IOLockUnlock(fSwapLock);
		queueReaders.releaseReaders();
		IOLog("lancelot-present-drop: swap=%u at=%llu queue_us=%llu gpu_wait_us=%llu\n",
		    swapWord(swap,SWAP_ID),micros(mach_absolute_time()),entered>=submitted?micros(entered-submitted):0,
		    micros(mach_absolute_time()-entered));
		return; /* no pixel algorithm, clearing, copying or record ran */
	}
	began = ready = mach_absolute_time();
	if (gOnTheBeat) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
