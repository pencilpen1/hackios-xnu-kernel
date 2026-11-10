struct minisurf_client_ref *
IOSurfaceRootUserClient::addRef(MiniIOSurface * surface)
{
	struct minisurf_client_ref * ref;
	IOMemoryMap * map;
	if (fClosing || fNextRefGeneration == UINT64_MAX) return NULL;

	if (fRefCount == fRefCapacity) {
		uint32_t capacity = fRefCapacity ? fRefCapacity * 2 : 16;
		struct minisurf_client_ref * refs = IONew(struct minisurf_client_ref, capacity);

		if (refs == NULL) {
			return NULL;
		}
		if (fRefs != NULL) {
			memcpy(refs, fRefs, fRefCount * sizeof(*refs));
			IODelete(fRefs, struct minisurf_client_ref, fRefCapacity);
		}
		fRefs = refs;
		fRefCapacity = capacity;
	}
	if (surface->maliCpuLease != NULL && surface->maliCpuLease->lockMapping(false) != LMAL_OK)
		return NULL;
	map = surface->memory->createMappingInTask(fTask, 0, kIOMapAnywhere);
	if (surface->maliCpuLease != NULL) surface->maliCpuLease->unlockMapping();
	if (map == NULL) {
		IOLog("MiniIOSurface: cannot map surface %u (%u bytes) into the client\n", surface->sid, surface->info.allocSize);
		return NULL;
	}
	ref = &fRefs[fRefCount++];
	bzero(ref, sizeof(*ref));
	ref->surface = surface;
	ref->maliGeneration = ++fNextRefGeneration;
	ref->map = map;
	if (surface->client != NULL && surface->clientTask != fTask) {
		ref->pixelMap = surface->client->createMappingInTask(fTask, 0, kIOMapAnywhere);
	}
	surfaceHold(surface);
	return ref;
}
