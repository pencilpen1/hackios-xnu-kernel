/*
 * MiniIOSurface: a stand-in for Apple's closed IOSurface.kext (see MiniIOSurface.h).
 *
 * A surface is a piece of wired kernel memory: one page of state that clients read directly,
 * followed by the pixels. Every client that knows a surface has the whole of it mapped once.
 * Surfaces travel between processes as Mach send rights (IOSurfaceSendRight) or by number.
 *
 * What is not here: child surfaces, wrapping of client memory, purgeability, compression,
 * notifications. Calls that are not implemented are logged once and refused.
 */

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/thread_call.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniIOSurface.h"
#include "LancelotMaliCpuLease.h"
extern "C" {
int lancelot_mali_queued_backend(void);
bool LancelotMaliQueueSurfaceWait(MiniIOSurface *, uint64_t);
bool LancelotMaliQueueSurfaceReadBegin(MiniIOSurface *, uint64_t);
void LancelotMaliQueueSurfaceReadEnd(MiniIOSurface *);
void LancelotMaliQueueSurfaceRelease(MiniIOSurface *);
}

static_assert(sizeof(struct minisurf_plane) == 0x80, "plane layout");
static_assert(sizeof(struct minisurf_result) == 0x68, "result layout");
static_assert(offsetof(struct minisurf_shared, attachments) == 0x30, "shared layout");
static_assert(sizeof(struct minisurf_result) + 20 * sizeof(struct minisurf_plane) == MINISURF_RESULT_MAX, "result size");

/* IOConnectCallMethod selectors of IOSurfaceRootUserClient (iOS 16.5) */
enum {
	kSurfaceMethodCreate            = 0,
	kSurfaceMethodLookup            = 4,
	kSurfaceMethodSetYCbCrMatrix    = 5,
	kSurfaceMethodWrapClientImage   = 6,
	kSurfaceMethodWrapClientMemory  = 7,
	kSurfaceMethodSetValue          = 9,
	kSurfaceMethodCopyValue         = 10,
	kSurfaceMethodRemoveValue       = 11,
	kSurfaceMethodBindAccel         = 12,
	kSurfaceMethodInit              = 13,
	kSurfaceMethodLog               = 19,
	kSurfaceMethodSetPurgeable      = 20,
	kSurfaceMethodSetOwnership      = 21,
	kSurfaceMethodSetTiled          = 22,
	kSurfaceMethodSetTimestamp      = 24,
	kSurfaceMethodSetBulkAttachments = 27,
	kSurfaceMethodCommPageAddress   = 32,
	kSurfaceMethodSetIndexedTimestamp = 33,
	kSurfaceMethodLookupFromPort    = 34,
	kSurfaceMethodCreatePort        = 35,
	kSurfaceMethodDetachModeCode    = 43,
	kSurfaceMethodSetOwnershipIdentity = 44,
};

/* IOConnectTrap indices */
enum {
	kSurfaceTrapIncrementUseCount   = 0,
	kSurfaceTrapDecrementUseCount   = 1,
	kSurfaceTrapLock                = 2,
	kSurfaceTrapUnlock              = 3,
	kSurfaceTrapDetach              = 4,    /* the client is about to release: forget it has the surface */
	kSurfaceTrapRelease             = 5,
	kSurfaceTrapCount
};

#define kSurfaceLockReadOnly    1u

#define MINISURF_TABLE_SIZE     16384
#define MINISURF_MAX_DIMENSION  16384
#define MINISURF_MAX_ALLOC      (512u << 20)
#define MINISURF_ROW_ALIGN      64

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static IOLock *         gSurfaceLock;
static MiniIOSurface *  gSurfaceTable[MINISURF_TABLE_SIZE];     /* index = surface number; [0] unused */
static uint32_t         gSurfaceNext = 1;
static uint32_t         gSurfaceCount;
static uint64_t         gSurfaceBytes;
static uint32_t         gSurfaceUnheld;         /* surfaces around client memory that could not be made (said a few times) */
/* what making surfaces and taking them away has cost since it was last asked (MiniIOSurfaceCost()) */
static uint64_t         gSurfaceMaking, gSurfaceFreeing;
static uint32_t         gSurfacesMade, gSurfacesFreed;
static uint64_t         gSurfaceLogged[2];                      /* selectors already complained about */
/* Boot-arg surftrace=N: log the first N surfaces created (default 24). */
static TUNABLE(uint32_t, gSurfaceTrace, "surftrace", 24);
static uint32_t         gSurfaceTraced;                         /* (a tunable is read-only after boot) */
/*
 * Boot-arg surftrace_of=NAME: only the surfaces made by programs whose name begins with NAME count for
 * surftrace (then up to N of theirs), and what becomes of them is said as well: the ports made for them
 * and who looks them up from a port (another program, which shows it: the window server for a layer).
 */
static char             gSurfaceTraceOf[32];

extern "C" void loader_log_line(const char *line);

/* A line about a traced surface: to the loader's log too, where it outlives the kernel's own, short log. */
static void
traceLine(const char * format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (gSurfaceTraceOf[0] != 0) {
		loader_log_line(line);
	}
	IOLog("%s", line);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDefineMetaClassAndStructors(MiniIOSurface, OSObject);

static uint64_t maliCpuWaitNs(uint64_t milliseconds)
{
	return milliseconds > LMAL_MAX_WAIT_NS / 1000000 ? LMAL_MAX_WAIT_NS : milliseconds * 1000000;
}
bool MiniIOSurfaceMaliDrawn(MiniIOSurface *surface, uint64_t milliseconds)
{
	if (!LancelotMaliQueueSurfaceWait(surface, maliCpuWaitNs(milliseconds))) return false;
	return surface == NULL || surface->maliCpuLease == NULL ||
	    surface->maliCpuLease->waitCpu(maliCpuWaitNs(milliseconds)) == LMAL_OK;
}
bool MiniIOSurfaceMaliReadBegin(MiniIOSurface *surface, uint64_t milliseconds)
{
	if (!LancelotMaliQueueSurfaceReadBegin(surface, maliCpuWaitNs(milliseconds))) return false;
	bool allowed = surface != NULL && (surface->maliCpuLease == NULL ||
	    surface->maliCpuLease->beginCpuRead(maliCpuWaitNs(milliseconds)) == LMAL_OK);
	if (!allowed) LancelotMaliQueueSurfaceReadEnd(surface);
	return allowed;
}
void MiniIOSurfaceMaliReadEnd(MiniIOSurface *surface)
{
	if (surface != NULL && surface->maliCpuLease != NULL) surface->maliCpuLease->endCpuRead();
	LancelotMaliQueueSurfaceReadEnd(surface);
}

/*
 * The memory of the big surfaces is kept when they go, a few pieces of it, and given to the
 * next surface of the same size. CoreAnimation makes a surface of the whole display for every
 * frame and frees one: 14 MB got, cleared, entered into the kernel's map and taken out of it
 * again, sixty times a second -- 2.8 ms to make and 2 ms to free each, a quarter of the time
 * there is for a frame, on the thread that draws it. A piece that is kept is cleared again
 * before it is given out: a new surface is all zeroes, whoever makes it. The clearing is done
 * by the display's driver between two frames (MiniIOSurfaceClearKept(): while it copies a
 * frame to the panel the memory is busy enough, and the copy took half as long again when
 * this ran beside it), or a moment later by a thread of this file's if no frames are going
 * out, or by whoever asks for the memory if that comes first.
 */
#define SURFACE_KEEP_FROM       (4u << 20)      /* bytes: smaller ones are not worth it */
#define SURFACE_KEPT            4
#define SURFACE_CLEAR_MS        40              /* kept memory is cleared this long after, if the display's driver has not done it */

static struct {
	IOBufferMemoryDescriptor *      memory;
	LancelotMaliCpuLease *          lease;
	bool                            clear;          /* cleared since it was last a surface's */
	bool                            busy;           /* being cleared right now */
	uint64_t                        since;          /* when it was kept */
} gSurfaceKept[SURFACE_KEPT];
static uint32_t         gSurfacesNotKept[2];    /* memory not kept: somebody else had it too; it put out a piece kept longer */
static IOLock *         gSurfaceKeptLock;
static thread_call_t    gSurfaceClearing;
static uint32_t         gSurfacesGivenAgain;    /* for MiniIOSurfaceCost() */
/* boot-arg surf_clear=0: kept memory is given out as it is (to see what the clearing costs; a surface is then not all zeroes) */
static TUNABLE(uint32_t, gSurfaceClear, "surf_clear", 1);

void
MiniIOSurfaceClearKept(void)
{
	if (gSurfaceKeptLock == NULL || !gSurfaceClear) {
		return;
	}
	IOLockLock(gSurfaceKeptLock);
	for (unsigned i = 0; i < SURFACE_KEPT; i++) {
		IOBufferMemoryDescriptor * memory = gSurfaceKept[i].memory;

		if (memory == NULL || gSurfaceKept[i].clear || gSurfaceKept[i].busy) {
			continue;
		}
		gSurfaceKept[i].busy = true;
		IOLockUnlock(gSurfaceKeptLock);
		bzero(memory->getBytesNoCopy(), memory->getCapacity());
		IOLockLock(gSurfaceKeptLock);
		gSurfaceKept[i].busy = false;
		gSurfaceKept[i].clear = true;
	}
	IOLockUnlock(gSurfaceKeptLock);
}

static void
surfaceMemoryClear(thread_call_param_t unused0 __unused, thread_call_param_t unused1 __unused)
{
	MiniIOSurfaceClearKept();
}

/* Memory for a surface: the state page and `bytes` after it, all zeroes. */
static IOBufferMemoryDescriptor *
surfaceMemory(vm_size_t bytes, bool *fresh, LancelotMaliCpuLease **lease)
{
	IOBufferMemoryDescriptor * memory = NULL;
	bool clear = false;
	*fresh = false; /* kept roots have exported history; never late-register */
	*lease = NULL;

	if (bytes >= SURFACE_KEEP_FROM && gSurfaceKeptLock != NULL) {
		IOLockLock(gSurfaceKeptLock);
		for (int pass = 0; pass < 2 && memory == NULL; pass++) {
			/* one that is clear already, if there is one */
			for (unsigned i = 0; i < SURFACE_KEPT && memory == NULL; i++) {
				if (gSurfaceKept[i].memory != NULL && !gSurfaceKept[i].busy && (pass == 1 || gSurfaceKept[i].clear) &&
				    gSurfaceKept[i].memory->getCapacity() == bytes) {
					memory = gSurfaceKept[i].memory;
					*lease = gSurfaceKept[i].lease;
					clear = gSurfaceKept[i].clear;
					gSurfaceKept[i].memory = NULL;
					gSurfaceKept[i].lease = NULL;
				}
			}
		}
		gSurfacesGivenAgain += memory != NULL;
		IOLockUnlock(gSurfaceKeptLock);
		if (memory != NULL && !clear) {
			bzero(memory->getBytesNoCopy(), gSurfaceClear ? bytes : PAGE_SIZE);
		}
	}
	if (memory == NULL) {
		/* Explicit WB also requests IOMD's persistent memory entry, allowing
		 * a fresh root lease without changing any existing page's cache mode. */
		IOOptionBits cache = (lancelot_hakios_backend() || lancelot_mali_queued_backend()) ? kIOMapCopybackCache : 0;
		memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared | cache, bytes,
		    PAGE_SIZE);
		*fresh = memory != NULL;
	}
	return memory;
}

/*
 * A surface is gone: its memory is kept for the next, if it is big and nobody else has it. When
 * all the places are taken, it takes the place of the piece kept longest: pieces of a size nobody
 * asks for again (a big picture made once) once stayed there for good, and then no frame's memory
 * was kept any more -- each made afresh and freed (2 to 4 ms, and 1 to 2) instead of 0.3 and 0.05.
 */
static void
surfaceMemoryBack(IOBufferMemoryDescriptor * memory, LancelotMaliCpuLease *lease)
{
	IOBufferMemoryDescriptor * evicted = NULL;
	LancelotMaliCpuLease *evictedLease = NULL;
	bool sole = lease ? lease->canRecycle(memory) :
	    (!lancelot_mali_queued_backend() && memory->getRetainCount() == 1);

	if (memory->getCapacity() >= SURFACE_KEEP_FROM && !sole) {
		OSIncrementAtomic(&gSurfacesNotKept[0]);
	}
	if (memory->getCapacity() >= SURFACE_KEEP_FROM && sole && gSurfaceKeptLock != NULL) {
		int oldest = -1;

		IOLockLock(gSurfaceKeptLock);
		for (unsigned i = 0; i < SURFACE_KEPT && memory != NULL; i++) {
			if (gSurfaceKept[i].memory == NULL) {
				gSurfaceKept[i].memory = memory;
				gSurfaceKept[i].lease = lease;lease = NULL;
				gSurfaceKept[i].clear = gSurfaceKept[i].busy = false;
				gSurfaceKept[i].since = mach_absolute_time();
				memory = NULL;
			} else if (!gSurfaceKept[i].busy && (oldest < 0 || gSurfaceKept[i].since < gSurfaceKept[oldest].since)) {
				oldest = (int) i;
			}
		}
		if (memory != NULL && oldest >= 0) {
			evicted = gSurfaceKept[oldest].memory;
			evictedLease = gSurfaceKept[oldest].lease;
			gSurfaceKept[oldest].memory = memory;
			gSurfaceKept[oldest].lease = lease;lease = NULL;
			gSurfaceKept[oldest].clear = gSurfaceKept[oldest].busy = false;
			gSurfaceKept[oldest].since = mach_absolute_time();
			memory = NULL;
			gSurfacesNotKept[1]++;
		}
		IOLockUnlock(gSurfaceKeptLock);
		OSSafeReleaseNULL(evictedLease);
		if (evicted != NULL) {
			evicted->release();
		}
		if (memory == NULL) {
			if (gSurfaceClear) {
				AbsoluteTime deadline;

				clock_interval_to_deadline(SURFACE_CLEAR_MS, kMillisecondScale, &deadline);
				thread_call_enter_delayed(gSurfaceClearing, deadline);
			}
			return;
		}
	}
	OSSafeReleaseNULL(lease);memory->release();
}

void
MiniIOSurface::free(void)
{
	uint64_t began = mach_absolute_time();

	if (traced && kpixels != NULL) {
		uint32_t nonzero = 0, middle = 0;

		for (uint32_t at = 0; at + 4 <= info.allocSize; at += 64) {
			nonzero += kpixels[at] != 0 || kpixels[at + 1] != 0 || kpixels[at + 2] != 0 || kpixels[at + 3] != 0;
		}
		memcpy(&middle, kpixels + (info.allocSize / 2 & ~3u), 4);
		traceLine("MiniIOSurface: surface %u (%ux%u) goes: of every 64 bytes %u of %u begin with a pixel that is not 0, one in "
		    "the middle %#010x\n", sid, info.width, info.height, nonzero, info.allocSize / 64, middle);
	}

	OSSafeReleaseNULL(values);
	LancelotMaliQueueSurfaceRelease(this);
	OSSafeReleaseNULL(clientMap);
	if (client != NULL) {
		client->complete();
		client->release();
		client = NULL;
	}
	if (memory != NULL) {
		surfaceMemoryBack(memory,maliCpuLease);maliCpuLease = NULL;
		memory = NULL;
	}
	OSSafeReleaseNULL(maliCpuLease);
	OSObject::free();
	OSAddAtomic64((SInt64) (mach_absolute_time() - began), (SInt64 *) &gSurfaceFreeing);
	OSIncrementAtomic(&gSurfacesFreed);
}

/* How many surfaces were made and freed since this was last asked, and the microseconds each took on average. */
void
MiniIOSurfaceCost(uint32_t * made, uint64_t * making, uint32_t * freed, uint64_t * freeing, uint32_t * again)
{
	uint64_t nanos;

	*again = OSBitAndAtomic(0, &gSurfacesGivenAgain);

	*made = OSBitAndAtomic(0, &gSurfacesMade);
	*freed = OSBitAndAtomic(0, &gSurfacesFreed);
	absolutetime_to_nanoseconds(gSurfaceMaking, &nanos);
	*making = *made ? nanos / NSEC_PER_USEC / *made : 0;
	absolutetime_to_nanoseconds(gSurfaceFreeing, &nanos);
	*freeing = *freed ? nanos / NSEC_PER_USEC / *freed : 0;
	gSurfaceMaking = gSurfaceFreeing = 0;
}

/* The values changed: a new seed, and how much room a client needs to fetch them. */
void
MiniIOSurface::updateValuesState(void)
{
	struct minisurf_shared * sh = shared();
	uint32_t size = 0;

	if (values != NULL && values->getCount() != 0) {
		OSSerialize * s = OSSerialize::binaryWithCapacity(4096);
		if (s != NULL) {
			if (values->serialize(s)) {
				size = s->getLength() + 8;
			}
			s->release();
		}
	}
	sh->valuesSeed = (sh->valuesSeed + 2) | 1;
	sh->valuesSize = size;
}

static uint32_t
dictNumber(OSDictionary * dict, const char * key, uint32_t missing)
{
	OSObject * obj = dict ? dict->getObject(key) : NULL;
	OSNumber * num = OSDynamicCast(OSNumber, obj);

	if (num != NULL) {
		return num->unsigned32BitValue();
	}
	if (obj == kOSBooleanTrue) {
		return 1;
	}
	if (obj == kOSBooleanFalse) {
		return 0;
	}
	return missing;
}

static uint32_t
alignUp(uint64_t value, uint32_t to)
{
	return (uint32_t) ((value + to - 1) / to * to);
}

/* Bytes per element when the client did not say. */
static uint32_t
defaultBytesPerElement(uint32_t format)
{
	switch (format) {
	case FOURCC('L', '0', '0', '8'):
	case FOURCC('A', '0', '0', '8'):
	case 0x00000008:
		return 1;
	case FOURCC('L', '5', '6', '5'):
	case FOURCC('B', '5', '6', '5'):
	case FOURCC('L', '5', '5', '5'):
	case FOURCC('2', 'v', 'u', 'y'):
	case FOURCC('y', 'u', 'v', 's'):
	case FOURCC('y', 'u', 'v', 'f'):
	case FOURCC('L', '0', '1', '6'):
	case FOURCC('L', '0', '0', 'h'):
		return 2;
	case FOURCC('R', 'G', 'h', 'A'):
		return 8;
	case FOURCC('R', 'G', 'f', 'A'):
		return 16;
	default:
		return 4;
	}
}

/*
 * Two-plane YCbCr formats: luma, then interleaved chroma.
 * Returns false for anything else. *wide: 10-bit samples in 16 bits.
 */
static bool
biplanarLayout(uint32_t format, uint32_t * hsub, uint32_t * vsub, bool * wide)
{
	switch (format) {
	case FOURCC('4', '2', '0', 'v'): case FOURCC('4', '2', '0', 'f'):
		*hsub = 2; *vsub = 2; *wide = false; return true;
	case FOURCC('4', '2', '2', 'v'): case FOURCC('4', '2', '2', 'f'):
		*hsub = 2; *vsub = 1; *wide = false; return true;
	case FOURCC('4', '4', '4', 'v'): case FOURCC('4', '4', '4', 'f'):
		*hsub = 1; *vsub = 1; *wide = false; return true;
	case FOURCC('x', '4', '2', '0'): case FOURCC('x', 'f', '2', '0'):
		*hsub = 2; *vsub = 2; *wide = true; return true;
	case FOURCC('x', '4', '2', '2'): case FOURCC('x', 'f', '2', '2'):
		*hsub = 2; *vsub = 1; *wide = true; return true;
	case FOURCC('x', '4', '4', '4'): case FOURCC('x', 'f', '4', '4'):
		*hsub = 1; *vsub = 1; *wide = true; return true;
	default:
		return false;
	}
}

static void
describeComponents(struct minisurf_plane * plane, uint32_t format)
{
	/* in memory order; names: 1 alpha, 2 red, 3 green, 4 blue */
	static const uint8_t bgra[4] = { 4, 3, 2, 1 };
	static const uint8_t rgba[4] = { 2, 3, 4, 1 };
	const uint8_t * names;

	if (format == FOURCC('B', 'G', 'R', 'A')) {
		names = bgra;
	} else if (format == FOURCC('R', 'G', 'B', 'A')) {
		names = rgba;
	} else {
		return;
	}
	plane->componentCount = 4;
	for (unsigned i = 0; i < 4; i++) {
		plane->componentBitDepth[i] = 8;
		plane->componentBitOffset[i] = (uint8_t) (8 * i);
		plane->componentName[i] = names[i];
		plane->componentType[i] = 1;            /* unsigned integer */
		plane->componentRange[i] = 1;           /* full range */
	}
}

/*
 * Make a surface from the properties a client gave to IOSurfaceCreate().
 * Returned with one holder, and entered in the table.
 */
static MiniIOSurface *
surfaceCreate(OSDictionary * props, IOReturn * error)
{
	MiniIOSurface * s;
	struct minisurf_result * info;
	uint32_t width, height, format, bpe, ew, eh, bpr, alloc, offset, planeCount = 0;
	uint32_t hsub, vsub;
	bool wide, onePlane = false;
	OSArray * planeInfo;
	OSNumber * wrapped;
	uint64_t needed;

	*error = kIOReturnBadArgument;
	width = dictNumber(props, "IOSurfaceWidth", 0);
	height = dictNumber(props, "IOSurfaceHeight", 0);
	format = dictNumber(props, "IOSurfacePixelFormat", 0);
	/*
	 * Ten bits a colour, packed into 32-bit pixels: what a device with a wide-gamut display
	 * (the iPhone X is one) asks for when it wants a picture of the screen, a snapshot for a
	 * transition for instance. CoreAnimation's software renderer does not draw into those
	 * ("SW renderer invalid destination format w30r": the transition comes out black), and
	 * there is no wide gamut on this panel anyway. The surface is made as ordinary BGRA of
	 * the same geometry, and says so: whoever uses it reads the format back from the surface.
	 */
	if (format == 'w30r' || format == 'l10r' || format == 'w30R') {
		format = 'BGRA';
	}
	/*
	 * The same with alpha in a plane of its own: what CoreAnimation makes the screen's own
	 * surfaces of once it draws with a GPU's renderer (OpenGL ES, userland/src/eagl.c) instead
	 * of its software one. One plane of BGRA here too; the framebuffer shows nothing else.
	 */
	if (format == 'b3a8') {
		format = 'BGRA';
		onePlane = true;
	}
	ew = dictNumber(props, "IOSurfaceElementWidth", 1);
	eh = dictNumber(props, "IOSurfaceElementHeight", 1);
	bpe = dictNumber(props, "IOSurfaceBytesPerElement", 0);
	offset = dictNumber(props, "IOSurfaceOffset", 0);
	if (width > MINISURF_MAX_DIMENSION || height > MINISURF_MAX_DIMENSION || ew == 0 || eh == 0 || ew > 255 || eh > 255) {
		return NULL;
	}
	if (props->getObject("IOSurfaceAddressRanges") != NULL) {
		IOLog("MiniIOSurface: wrapping client memory in pieces is not supported\n");
		*error = kIOReturnUnsupported;
		return NULL;
	}
	/*
	 * A surface around memory the client has already: CoreAnimation's OpenGL ES renderer makes
	 * them around pictures it has in memory (an application's icon, a layer's contents as the
	 * application has drawn them), to hand them to the GPU as they are. The surface's pixels
	 * are that memory itself, not a copy: the picture goes on being drawn into after the
	 * surface is made, and whoever looks at the surface is to see what is there now.
	 */
	wrapped = OSDynamicCast(OSNumber, props->getObject("IOSurfaceAddress"));

	s = OSTypeAlloc(MiniIOSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		*error = kIOReturnNoMemory;
		return NULL;
	}
	info = &s->info;

	planeInfo = onePlane ? NULL : OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	needed = 0;
	if (planeInfo != NULL && planeInfo->getCount() != 0) {
		planeCount = planeInfo->getCount();
		if (planeCount > MINISURF_MAX_PLANES) {
			IOLog("MiniIOSurface: %u planes asked for, %u supported\n", planeCount, MINISURF_MAX_PLANES);
			s->release();
			return NULL;
		}
		for (uint32_t i = 0; i < planeCount; i++) {
			OSDictionary * pd = OSDynamicCast(OSDictionary, planeInfo->getObject(i));
			struct minisurf_plane * p = &s->planes[i];
			uint32_t pew = dictNumber(pd, "IOSurfacePlaneElementWidth", 1);
			uint32_t peh = dictNumber(pd, "IOSurfacePlaneElementHeight", 1);

			if (pew == 0 || peh == 0 || pew > 255 || peh > 255) {
				s->release();
				return NULL;
			}
			p->width = dictNumber(pd, "IOSurfacePlaneWidth", width);
			p->height = dictNumber(pd, "IOSurfacePlaneHeight", height);
			p->bytesPerElement = (uint16_t) dictNumber(pd, "IOSurfacePlaneBytesPerElement", bpe ? bpe : 1);
			p->elementWidth = (uint8_t) pew;
			p->elementHeight = (uint8_t) peh;
			p->bytesPerRow = dictNumber(pd, "IOSurfacePlaneBytesPerRow", 0);
			if (p->bytesPerRow == 0) {
				p->bytesPerRow = alignUp((uint64_t) ((p->width + pew - 1) / pew) * p->bytesPerElement, MINISURF_ROW_ALIGN);
			}
			p->size = dictNumber(pd, "IOSurfacePlaneSize", 0);
			if (p->size == 0) {
				p->size = p->bytesPerRow * ((p->height + peh - 1) / peh);
			}
			p->offset = dictNumber(pd, "IOSurfacePlaneOffset", dictNumber(pd, "IOSurfacePlaneBase", (uint32_t) needed));
			p->extendedPixels[0] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnLeft", 0);
			p->extendedPixels[1] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnRight", 0);
			p->extendedPixels[2] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnTop", 0);
			p->extendedPixels[3] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnBottom", 0);
			if ((uint64_t) p->offset + p->size > needed) {
				needed = (uint64_t) p->offset + p->size;
			}
		}
	} else if (biplanarLayout(format, &hsub, &vsub, &wide) && width != 0 && height != 0) {
		struct minisurf_plane * y = &s->planes[0], * c = &s->planes[1];

		planeCount = 2;
		y->width = width;
		y->height = height;
		y->bytesPerElement = wide ? 2 : 1;
		y->elementWidth = y->elementHeight = 1;
		y->bytesPerRow = alignUp((uint64_t) width * y->bytesPerElement, MINISURF_ROW_ALIGN);
		y->size = y->bytesPerRow * height;
		y->offset = 0;
		c->width = (width + hsub - 1) / hsub;
		c->height = (height + vsub - 1) / vsub;
		c->bytesPerElement = wide ? 4 : 2;
		c->elementWidth = c->elementHeight = 1;
		c->bytesPerRow = alignUp((uint64_t) c->width * c->bytesPerElement, MINISURF_ROW_ALIGN);
		c->size = c->bytesPerRow * c->height;
		c->offset = y->size;
		needed = (uint64_t) c->offset + c->size;
	}

	if (bpe == 0) {
		bpe = planeCount ? s->planes[0].bytesPerElement : defaultBytesPerElement(format);
	}
	bpr = dictNumber(props, "IOSurfaceBytesPerRow", 0);
	if (bpr == 0) {
		bpr = planeCount ? s->planes[0].bytesPerRow
		    : alignUp((uint64_t) ((width + ew - 1) / ew) * bpe, MINISURF_ROW_ALIGN);
	}
	if (planeCount == 0) {
		needed = (uint64_t) bpr * ((height + eh - 1) / eh);
		/* the one implicit plane, for the per-plane getters */
		s->planes[0].width = width;
		s->planes[0].height = height;
		s->planes[0].bytesPerRow = bpr;
		s->planes[0].bytesPerElement = (uint16_t) bpe;
		s->planes[0].elementWidth = (uint8_t) ew;
		s->planes[0].elementHeight = (uint8_t) eh;
		s->planes[0].size = (uint32_t) needed;
		describeComponents(&s->planes[0], format);
	}
	needed += offset;
	alloc = dictNumber(props, "IOSurfaceAllocSize", 0);
	if (alloc < needed) {
		if (alloc != 0) {
			IOLog("MiniIOSurface: alloc size %u too small for %ux%u '%.4s', using %llu\n", alloc, width, height,
			    (const char *) &format, needed);
		}
		if (needed > MINISURF_MAX_ALLOC) {
			s->release();
			return NULL;
		}
		alloc = (uint32_t) needed;
	}
	if (alloc == 0 || alloc > MINISURF_MAX_ALLOC) {
		s->release();
		return NULL;
	}
	if (wrapped == NULL) {
		alloc = alignUp(alloc, (uint32_t) PAGE_SIZE);
	} else {
		/* (the memory is as big as its owner says, and not a byte of ours is added to it) */
		alloc = dictNumber(props, "IOSurfaceAllocSize", (uint32_t) needed);
	}

	bool freshMemory = false;
	s->memory = surfaceMemory((vm_size_t) PAGE_SIZE + (wrapped ? 0 : alloc), &freshMemory, &s->maliCpuLease);
	if (s->memory == NULL) {
		IOLog("MiniIOSurface: no memory for %u bytes (%u surfaces, %llu MB in use)\n", alloc, gSurfaceCount,
		    gSurfaceBytes >> 20);
		s->release();
		*error = kIOReturnNoMemory;
		return NULL;
	}
	s->kbase = (uint8_t *) s->memory->getBytesNoCopy();
	if (wrapped == NULL) {
		struct lmal_cpu_lease_why why={};
		if(!s->maliCpuLease)s->maliCpuLease = LancelotMaliCpuLease::withRoot(s->memory, freshMemory, 1, &why);
		if(!s->maliCpuLease && (lancelot_mali_queued_backend() || lancelot_hakios_backend()))
			IOLog("lancelot-surface-lease: sid=%u fresh=%u bytes=%llu step=%u detail=%#x value=%llu\n",
			    s->sid,freshMemory,(uint64_t)s->memory->getLength(),why.step,why.detail,why.value);
		if (s->maliCpuLease != NULL)
			s->kbase = (uint8_t *) s->maliCpuLease->cacheAddress();
	}
	s->values = OSDictionary::withCapacity(4);
	s->kpixels = s->kbase + PAGE_SIZE;
	if (wrapped != NULL) {
		s->clientTask = current_task();
		s->clientAddress = wrapped->unsigned64BitValue();
		IOReturn held = kIOReturnNoMemory;

		/* (held for writing if it can be written, which a picture that is only shown need not be) */
		for (unsigned attempt = 0; attempt < 2 && held != kIOReturnSuccess; attempt++) {
			OSSafeReleaseNULL(s->client);
			s->client = IOMemoryDescriptor::withAddressRange(s->clientAddress, alloc,
			    attempt == 0 ? kIODirectionInOut : kIODirectionOut, s->clientTask);
			held = s->client != NULL ? s->client->prepare() : kIOReturnNoMemory;
		}
		if (held == kIOReturnSuccess) {
			s->clientMap = s->client->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
		} else {
			OSSafeReleaseNULL(s->client);
		}
		if (s->clientMap == NULL) {
			if (gSurfaceUnheld++ < 8) {
				IOLog("MiniIOSurface: the memory to make a surface around (%u bytes at %#llx) cannot be %s (%#x)\n", alloc,
				    s->clientAddress, held == kIOReturnSuccess ? "seen from here" : "held", held);
			}
			s->release();
			*error = kIOReturnVMError;
			return NULL;
		}
		s->kpixels = (uint8_t *) s->clientMap->getVirtualAddress();
	}

	info->allocSize = alloc;
	info->width = width;
	info->height = height;
	info->bytesPerRow = bpr;
	info->offset = offset;
	info->pixelFormat = format;
	info->planeCount = planeCount;
	info->bytesPerElement = (uint16_t) bpe;
	info->elementWidth = (uint8_t) ew;
	info->elementHeight = (uint8_t) eh;
	info->cacheMode = dictNumber(props, "IOSurfaceCacheMode", 0);
	info->subsampling = (uint8_t) dictNumber(props, "IOSurfaceSubsampling", 0);
	info->prohibitUseCount = (uint8_t) dictNumber(props, "IOSurfaceProhibitUseCount", 0);
	info->sliceCount = dictNumber(props, "IOSurfaceSliceCount", 0);
	info->protectionOptions = dictNumber(props, "IOSurfaceProtectionOptions", 0);

	IOLockLock(gSurfaceLock);
	uint32_t sid = 0;
	for (uint32_t tries = 0; tries < MINISURF_TABLE_SIZE - 1; tries++) {
		uint32_t candidate = gSurfaceNext;

		gSurfaceNext = (gSurfaceNext + 1 < MINISURF_TABLE_SIZE) ? gSurfaceNext + 1 : 1;
		if (gSurfaceTable[candidate] == NULL) {
			sid = candidate;
			break;
		}
	}
	if (sid == 0) {
		IOLockUnlock(gSurfaceLock);
		IOLog("MiniIOSurface: the surface table is full\n");
		s->release();
		*error = kIOReturnNoResources;
		return NULL;
	}
	s->sid = sid;
	s->holders = 1;
	info->sid = sid;
	info->traceID = sid;
	gSurfaceTable[sid] = s;                 /* the table owns the object until the last holder goes */
	gSurfaceCount++;
	gSurfaceBytes += alloc;

	struct minisurf_shared * sh = s->shared();
	sh->registryID = 0x100000000ULL + sid;
	sh->seed = 1;
	sh->valuesSeed = 1;
	sh->pixelSizeCasting = (uint8_t) dictNumber(props, "IOSurfacePixelSizeCastingAllowed", 1);
	sh->displayable = 1;
	sh->sysMemOnly = (uint8_t) dictNumber(props, "IOSurfaceSysMemOnly", 0);
	IOLockUnlock(gSurfaceLock);

	*error = kIOReturnSuccess;
	return s;
}

/* With the lock held. */
static void
surfaceHold(MiniIOSurface * s)
{
	s->holders++;
}

/* With the lock held. The caller must not touch the surface afterwards. */
static void
surfaceUnhold(MiniIOSurface * s)
{
	if (--s->holders != 0) {
		return;
	}
	gSurfaceTable[s->sid] = NULL;
	gSurfaceCount--;
	gSurfaceBytes -= s->info.allocSize;
	s->release();
}

MiniIOSurface *
MiniIOSurfaceCopy(uint32_t sid)
{
	MiniIOSurface * s = NULL;

	if (gSurfaceLock == NULL || sid == 0 || sid >= MINISURF_TABLE_SIZE) {
		return NULL;
	}
	IOLockLock(gSurfaceLock);
	s = gSurfaceTable[sid];
	if (s != NULL) {
		surfaceHold(s);
	}
	IOLockUnlock(gSurfaceLock);
	return s;
}

/* How many surfaces there are and how much (wired) memory they hold: for the kernel's memory report. */
extern "C" void
MiniIOSurfaceUsage(uint32_t * count, uint64_t * bytes)
{
	*count = gSurfaceCount;
	*bytes = gSurfaceBytes;
}

/* Publish display ownership to IOSurfaceIsInUse until every CPU row is read. */
bool MiniIOSurfaceDisplayRetain(MiniIOSurface *s)
{
    if(!s)return false;
    IOLockLock(gSurfaceLock);
    bool ok=s->useCount[1]!=UINT32_MAX && s->displayUseCount!=UINT32_MAX;
    if(ok){++s->useCount[1];++s->displayUseCount;s->shared()->useCount=(uint64_t)s->useCount[1]<<32|s->useCount[0];}
    IOLockUnlock(gSurfaceLock);return ok;
}
void MiniIOSurfaceDisplayRelease(MiniIOSurface *s)
{
    if(!s)return;
    IOLockLock(gSurfaceLock);
    if(s->displayUseCount){--s->displayUseCount;--s->useCount[1];s->shared()->useCount=(uint64_t)s->useCount[1]<<32|s->useCount[0];}
    IOLockUnlock(gSurfaceLock);
}

void
MiniIOSurfaceDrop(MiniIOSurface * s)
{
	if (s == NULL) {
		return;
	}
	IOLockLock(gSurfaceLock);
	surfaceUnhold(s);
	IOLockUnlock(gSurfaceLock);
}

MiniIOSurface *
MiniIOSurfaceCreateSimple(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
	OSDictionary * props = OSDictionary::withCapacity(4);
	MiniIOSurface * s = NULL;
	IOReturn error;

	if (props == NULL || gSurfaceLock == NULL) {
		OSSafeReleaseNULL(props);
		return NULL;
	}
	OSNumber * n;
	n = OSNumber::withNumber(width, 32); props->setObject("IOSurfaceWidth", n); n->release();
	n = OSNumber::withNumber(height, 32); props->setObject("IOSurfaceHeight", n); n->release();
	n = OSNumber::withNumber(pixelFormat, 32); props->setObject("IOSurfacePixelFormat", n); n->release();
	s = surfaceCreate(props, &error);
	props->release();
	return s;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* What a Mach send right for a surface points at. Holds the surface while any right exists. */
class IOSurfaceSendRight : public OSObject
{
	OSDeclareDefaultStructors(IOSurfaceSendRight);

public:
	MiniIOSurface * surface;

	virtual bool
	init(void) APPLE_KEXT_OVERRIDE
	{
		return OSObject::init();
	}
	virtual void free(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOSurfaceSendRight, OSObject);

void
IOSurfaceSendRight::free(void)
{
	MiniIOSurfaceDrop(surface);
	surface = NULL;
	OSObject::free();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOCoreSurfaceRoot : public IOService
{
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	setName("IOCoreSurfaceRoot");
	registerService();
	return true;
}

void
MiniIOSurfaceStart(IOService * provider)
{
	IOCoreSurfaceRoot * root;

	gSurfaceLock = IOLockAlloc();
	gSurfaceKeptLock = IOLockAlloc();
	if (!PE_parse_boot_arg_str("surftrace_of", gSurfaceTraceOf, sizeof(gSurfaceTraceOf))) {
		gSurfaceTraceOf[0] = 0;
	}
	gSurfaceClearing = thread_call_allocate(surfaceMemoryClear, NULL);
	root = OSTypeAlloc(IOCoreSurfaceRoot);
	if (root == NULL || !root->init() || !root->attach(provider)) {
		OSSafeReleaseNULL(root);
		IOLog("MiniIOSurface: cannot create the root service\n");
		return;
	}
	if (!root->start(provider)) {
		root->detach(provider);
		IOLog("MiniIOSurface: the root service did not start\n");
	}
	root->release();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* One surface as one client has it. */
struct minisurf_client_ref {
	MiniIOSurface * surface;                /* held */
	IOMemoryMap *   map;                    /* the whole of it in the client's address space */
	IOMemoryMap *   pixelMap;               /* ... the pixels, where those are another client's memory */
	uint32_t        use[2];                 /* what this client added to the use counts */
	uint32_t        maliReads, maliWrites;  /* nested CPU locks, one root lease per kind */
	uint64_t        maliGeneration;        /* survives array moves, never reused */
	bool            maliPending;
	bool            detached;               /* the client let go of it and will release shortly */
};

class IOSurfaceRootUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t                          fTask;
	struct minisurf_client_ref *    fRefs;
	uint32_t                        fRefCount;
	uint32_t                        fRefCapacity;
	uint64_t                        fNextRefGeneration;
	bool                            fClosing;
	IOExternalTrap                  fTraps[kSurfaceTrapCount];

	struct minisurf_client_ref * findRef(uint32_t sid, bool detached);
	struct minisurf_client_ref * addRef(MiniIOSurface * surface, bool * known = NULL);
	void dropRef(struct minisurf_client_ref * ref);
	IOReturn returnSurface(struct minisurf_client_ref * ref, bool known, IOExternalMethodArguments * args);

	IOReturn methodInit(IOExternalMethodArguments * args);
	IOReturn methodCreate(IOExternalMethodArguments * args);
	IOReturn methodLookup(IOExternalMethodArguments * args);
	IOReturn methodLookupFromPort(IOExternalMethodArguments * args);
	IOReturn methodCreatePort(IOExternalMethodArguments * args);
	IOReturn methodSetValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodCopyValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodRemoveValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodSetBulkAttachments(const uint8_t * in, size_t inSize);

	IOReturn trapIncrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapDecrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapLock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapUnlock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapDetach(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapRelease(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn lockCommon(uint32_t sid, uint32_t options, user_addr_t seedOut, bool unlock);
	IOReturn useCountCommon(uint32_t sid, uint32_t category, int delta);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
	virtual IOExternalTrap * getTargetAndTrapForIndex(IOService ** targetP, UInt32 index) APPLE_KEXT_OVERRIDE;
	static mach_port_name_t makeSendRight(task_t task, MiniIOSurface * surface);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	fClosing = false;
	return true;
}

bool
IOSurfaceRootUserClient::start(IOService * provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fTraps[kSurfaceTrapIncrementUseCount].func = (IOTrap) & IOSurfaceRootUserClient::trapIncrementUseCount;
	fTraps[kSurfaceTrapDecrementUseCount].func = (IOTrap) & IOSurfaceRootUserClient::trapDecrementUseCount;
	fTraps[kSurfaceTrapLock].func = (IOTrap) & IOSurfaceRootUserClient::trapLock;
	fTraps[kSurfaceTrapUnlock].func = (IOTrap) & IOSurfaceRootUserClient::trapUnlock;
	fTraps[kSurfaceTrapDetach].func = (IOTrap) & IOSurfaceRootUserClient::trapDetach;
	fTraps[kSurfaceTrapRelease].func = (IOTrap) & IOSurfaceRootUserClient::trapRelease;
	for (unsigned i = 0; i < kSurfaceTrapCount; i++) {
		fTraps[i].object = this;
	}
	return true;
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(gSurfaceLock);
	fClosing = true;
	while (fRefCount != 0) {
		dropRef(&fRefs[fRefCount - 1]);
	}
	IOLockUnlock(gSurfaceLock);
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOSurfaceRootUserClient::free(void)
{
	if (fRefs != NULL) {
		if (gSurfaceLock != NULL) {
			IOLockLock(gSurfaceLock);
			while (fRefCount != 0) {
				dropRef(&fRefs[fRefCount - 1]);
			}
			IOLockUnlock(gSurfaceLock);
		}
		IODelete(fRefs, struct minisurf_client_ref, fRefCapacity);
		fRefs = NULL;
	}
	IOUserClient::free();
}

/* All three enter and return with the lock held. addRef drops it while
 * preparing mappings; its caller must retain the client across that wait. */
struct minisurf_client_ref *
IOSurfaceRootUserClient::findRef(uint32_t sid, bool detached)
{
	for (uint32_t i = 0; i < fRefCount; i++) {
		if (fRefs[i].surface->sid == sid && fRefs[i].detached == detached) {
			return &fRefs[i];
		}
	}
	return NULL;
}

struct minisurf_client_ref *
IOSurfaceRootUserClient::addRef(MiniIOSurface * surface, bool * known)
{
	struct minisurf_client_ref * ref = NULL;
	IOMemoryMap * map = NULL, * pixelMap = NULL;
	if (known != NULL) *known = false;
	if (fClosing || fNextRefGeneration == UINT64_MAX) return NULL;

	/* GPU preparation can own one root while waiting for a CPU writer on
	 * another. That writer needs gSurfaceLock to unlock/close. Never keep
	 * the table locked while waiting for a GPU reservation or creating VM
	 * mappings. Hold the surface itself, not an fRefs pointer: concurrent
	 * lookup, detach and close can replace or move the entire ref array. */
	surfaceHold(surface);
	IOLockUnlock(gSurfaceLock);
	int mappingError = surface->maliCpuLease != NULL ? surface->maliCpuLease->lockMapping(false) : LMAL_OK;
	bool needsPixelMap = surface->client != NULL && surface->clientTask != fTask;
	if (mappingError == LMAL_OK) {
		map = surface->memory->createMappingInTask(fTask, 0, kIOMapAnywhere);
		if (map != NULL && needsPixelMap) {
			pixelMap = surface->client->createMappingInTask(fTask, 0, kIOMapAnywhere);
		}
		if (surface->maliCpuLease != NULL) surface->maliCpuLease->unlockMapping();
	}
	IOLockLock(gSurfaceLock);
	if (fClosing) goto out;
	/* A competing lookup may have published a ref during our wait. Reuse
	 * it, including its generation and CPU counts; discard only our maps. */
	ref = findRef(surface->sid, false);
	if (ref != NULL) {
		if (known != NULL) *known = true;
		goto out;
	}
	if (map == NULL || (needsPixelMap && pixelMap == NULL)) {
		IOLog("MiniIOSurface: cannot map surface %u (%u bytes) into the client, lease error %d\n",
		    surface->sid, surface->info.allocSize, mappingError);
		goto out;
	}
	if (fNextRefGeneration == UINT64_MAX) goto out;
	if (fRefCount == fRefCapacity) {
		if (fRefCapacity > UINT32_MAX / 2) goto out;
		uint32_t capacity = fRefCapacity ? fRefCapacity * 2 : 16;
		struct minisurf_client_ref * refs = IONew(struct minisurf_client_ref, capacity);

		if (refs == NULL) {
			goto out;
		}
		if (fRefs != NULL) {
			memcpy(refs, fRefs, fRefCount * sizeof(*refs));
			IODelete(fRefs, struct minisurf_client_ref, fRefCapacity);
		}
		fRefs = refs;
		fRefCapacity = capacity;
	}
	ref = &fRefs[fRefCount++];
	bzero(ref, sizeof(*ref));
	ref->surface = surface;
	ref->maliGeneration = ++fNextRefGeneration;
	ref->map = map;
	ref->pixelMap = pixelMap;
	return ref;                            /* transfer the temporary surface hold */

out:
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(pixelMap);
	surfaceUnhold(surface);
	return ref;
}

void
IOSurfaceRootUserClient::dropRef(struct minisurf_client_ref * ref)
{
	MiniIOSurface * s = ref->surface;
	if (s->maliCpuLease != NULL) {
		if (ref->maliReads) s->maliCpuLease->endUserCpu(fTask, false);
		if (ref->maliWrites) s->maliCpuLease->endUserCpu(fTask, true);
	}

	for (unsigned c = 0; c < 2; c++) {
		s->useCount[c] -= ref->use[c];
	}
	s->shared()->useCount = (uint64_t) s->useCount[1] << 32 | s->useCount[0];
	OSSafeReleaseNULL(ref->map);
	OSSafeReleaseNULL(ref->pixelMap);
	*ref = fRefs[--fRefCount];
	surfaceUnhold(s);
}

static uint32_t
resultSize(const MiniIOSurface * s)
{
	uint32_t planes = s->info.planeCount ? s->info.planeCount : 1;

	return (uint32_t) (sizeof(struct minisurf_result) + planes * sizeof(struct minisurf_plane));
}

/* Describe the surface to the client. known: it has the surface already, do not build a second view. */
IOReturn
IOSurfaceRootUserClient::returnSurface(struct minisurf_client_ref * ref, bool known, IOExternalMethodArguments * args)
{
	MiniIOSurface * s = ref->surface;
	uint32_t size = resultSize(s);
	struct minisurf_result * r = (struct minisurf_result *) args->structureOutput;
	mach_vm_address_t address = ref->map->getAddress();

	if (r == NULL || args->structureOutputSize < size) {
		return kIOReturnBadArgument;
	}
	*r = s->info;
	memcpy(r + 1, s->planes, size - sizeof(*r));
	r->shared = address;
	r->aux = address + MINISURF_AUX_OFFSET;
	r->base = known ? ~0ULL : s->client == NULL ? address + PAGE_SIZE :
	    ref->pixelMap != NULL ? ref->pixelMap->getAddress() : s->clientAddress;
	args->structureOutputSize = size;
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodInit(IOExternalMethodArguments * args)
{
	struct {
		uint32_t allocAlignMask;
		uint32_t rowAlignMask;
		uint32_t maxBytesPerRow;
		uint32_t maxWidth;
		uint32_t maxHeight;
		uint16_t reserved;
		uint16_t version;
	} reply;

	static_assert(sizeof(reply) == 0x18, "init reply");
	if (args->structureOutput == NULL || args->structureOutputSize < sizeof(reply)) {
		return kIOReturnBadArgument;
	}
	reply.allocAlignMask = (uint32_t) PAGE_MASK;
	reply.rowAlignMask = MINISURF_ROW_ALIGN - 1;
	reply.maxBytesPerRow = MINISURF_MAX_DIMENSION * 16;
	reply.maxWidth = MINISURF_MAX_DIMENSION;
	reply.maxHeight = MINISURF_MAX_DIMENSION;
	reply.reserved = 0;
	reply.version = 1;
	memcpy(args->structureOutput, &reply, sizeof(reply));
	args->structureOutputSize = sizeof(reply);
	return kIOReturnSuccess;
}

/* The serialized object in a request, or NULL. */
static OSObject *
unserialize(const uint8_t * data, size_t size)
{
	if (data == NULL || size < 4) {
		return NULL;
	}
	return OSUnserializeXML((const char *) data, size, NULL);
}

IOReturn
IOSurfaceRootUserClient::methodCreate(IOExternalMethodArguments * args)
{
	IOReturn ret;
	IOMemoryMap * inMap = NULL;
	const uint8_t * in = (const uint8_t *) args->structureInput;
	size_t inSize = args->structureInputSize;
	OSObject * obj;
	OSDictionary * props;
	MiniIOSurface * s;
	struct minisurf_client_ref * ref;
	uint64_t began = mach_absolute_time();

	if (args->scalarInputCount >= 1 && args->scalarInput[0] != 0) {
		IOLog("MiniIOSurface: child surfaces are not supported (parent %llu)\n", args->scalarInput[0]);
		return kIOReturnUnsupported;
	}
	if (args->structureInputDescriptor != NULL) {
		inMap = args->structureInputDescriptor->map(kIOMapReadOnly);
		if (inMap == NULL) {
			return kIOReturnNoMemory;
		}
		in = (const uint8_t *) inMap->getVirtualAddress();
		inSize = inMap->getLength();
	}
	obj = unserialize(in, inSize);
	OSSafeReleaseNULL(inMap);
	props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	uint64_t made_at = mach_absolute_time();
	s = surfaceCreate(props, &ret);
	made_at = mach_absolute_time() - made_at;
	if (s == NULL) {
		IOLog("MiniIOSurface: create failed 0x%x: %ux%u format 0x%x alloc %u\n", ret,
		    dictNumber(props, "IOSurfaceWidth", 0), dictNumber(props, "IOSurfaceHeight", 0),
		    dictNumber(props, "IOSurfacePixelFormat", 0), dictNumber(props, "IOSurfaceAllocSize", 0));
		props->release();
		return ret;
	}
	{
		char procname[32] = "?";
		static unsigned keysSaid;

		proc_selfname(procname, sizeof(procname));
		/* what a traced program asks for, key by key (the first few surfaces) */
		if (gSurfaceTraceOf[0] != 0 && strncmp(procname, gSurfaceTraceOf, strlen(gSurfaceTraceOf)) == 0 && keysSaid++ < 6) {
			OSCollectionIterator * keys = OSCollectionIterator::withCollection(props);
			char line[256];
			size_t used = 0;
			OSSymbol * key;

			line[0] = 0;
			while (keys != NULL && (key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL && used < sizeof(line)) {
				OSNumber * n = OSDynamicCast(OSNumber, props->getObject(key));

				used += (size_t) snprintf(line + used, sizeof(line) - used, " %s=%lld", key->getCStringNoCopy(),
				    n ? (long long) n->unsigned64BitValue() : -1LL);
			}
			OSSafeReleaseNULL(keys);
			traceLine("MiniIOSurface: %s asks for a surface with%s\n", procname, line);
		}
	}
	props->release();
	{
		char procname[32] = "?";

		proc_selfname(procname, sizeof(procname));
		if (gSurfaceTraceOf[0] != 0 && strncmp(procname, gSurfaceTraceOf, strlen(gSurfaceTraceOf)) != 0) {
			procname[0] = 0;        /* (not one of those asked about: not counted) */
		}
		if (procname[0] != 0 && gSurfaceTraced < gSurfaceTrace) {
			s->traced = gSurfaceTraceOf[0] != 0;
		}
	}
	if (s->traced || (gSurfaceTraceOf[0] == 0 && gSurfaceTraced < gSurfaceTrace)) {
		char procname[32] = "?";

		gSurfaceTraced++;
		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] created surface %u: %ux%u '%c%c%c%c', %u bytes per row, %u planes, %u bytes\n",
		    procname, proc_selfpid(), s->sid, s->info.width, s->info.height, (int) (s->info.pixelFormat >> 24 & 0xff),
		    (int) (s->info.pixelFormat >> 16 & 0xff), (int) (s->info.pixelFormat >> 8 & 0xff),
		    (int) (s->info.pixelFormat & 0xff), s->info.bytesPerRow, s->info.planeCount, s->info.allocSize);
	}

	uint64_t mapped_at = mach_absolute_time();
	bool known = false;
	bool bigSurface = s->info.allocSize >= SURFACE_KEEP_FROM;
	retain();
	IOLockLock(gSurfaceLock);
	ref = addRef(s, &known);
	ret = ref ? returnSurface(ref, false, args) : kIOReturnNoMemory;
	if (ref != NULL && !known && ret != kIOReturnSuccess) {
		dropRef(ref);
	}
	surfaceUnhold(s);                       /* the creator's hold; the client's own is in the ref */
	IOLockUnlock(gSurfaceLock);
	release();
	mapped_at = mach_absolute_time() - mapped_at;
	OSAddAtomic64((SInt64) (mach_absolute_time() - began), (SInt64 *) &gSurfaceMaking);
	OSIncrementAtomic(&gSurfacesMade);
	if (bigSurface) {
		/* the big ones (the window server's of the whole display): where the time to make one goes, every 5 s */
		static uint64_t since, all, making, mapping;
		static uint32_t count;
		uint64_t now = mach_absolute_time(), ns;

		count++;
		all += now - began;
		making += made_at;
		mapping += mapped_at;
		absolutetime_to_nanoseconds(now - since, &ns);
		if (since == 0) {
			since = now;
		} else if (ns >= 5 * NSEC_PER_SEC) {
			uint64_t a, m, p;

			absolutetime_to_nanoseconds(all, &a);
			absolutetime_to_nanoseconds(making, &m);
			absolutetime_to_nanoseconds(mapping, &p);
			IOLog("MiniIOSurface: %u big surfaces made, %llu us each: %llu getting the memory and the surface, %llu mapping it "
			    "for its maker; memory not kept as somebody else had it %u times, a piece kept longer put out for it %u times\n",
			    count, a / 1000 / count, m / 1000 / count, p / 1000 / count, OSBitAndAtomic(0, &gSurfacesNotKept[0]),
			    OSBitAndAtomic(0, &gSurfacesNotKept[1]));
			since = now;
			count = 0;
			all = making = mapping = 0;
		}
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodLookup(IOExternalMethodArguments * args)
{
	IOReturn ret = kIOReturnNotFound;
	uint32_t sid;
	struct minisurf_client_ref * ref;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	sid = (uint32_t) args->scalarInput[0];
	retain();
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref == NULL && sid != 0 && sid < MINISURF_TABLE_SIZE && gSurfaceTable[sid] != NULL) {
		ref = addRef(gSurfaceTable[sid]);
		if (ref == NULL) {
			ret = kIOReturnNoMemory;
		}
	}
	if (ref != NULL) {
		ret = returnSurface(ref, false, args);
	}
	IOLockUnlock(gSurfaceLock);
	release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodLookupFromPort(IOExternalMethodArguments * args)
{
	IOReturn ret;
	OSObject * obj = NULL;
	IOSurfaceSendRight * right;
	struct minisurf_client_ref * ref;
	bool known;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	ret = copyObjectForPortNameInTask(fTask, (mach_port_name_t) args->scalarInput[0], &obj);
	right = OSDynamicCast(IOSurfaceSendRight, obj);
	if (ret != kIOReturnSuccess || right == NULL || right->surface == NULL) {
		static unsigned said;

		if (said++ < 16) {
			char procname[32] = "?";

			proc_selfname(procname, sizeof(procname));
			traceLine("MiniIOSurface: %s[%d] looked up a surface from port %#llx: %s\n", procname, proc_selfpid(),
			    args->scalarInput[0], ret != kIOReturnSuccess ? "no object behind the port" : obj ? "not a surface's port" :
			    "no surface");
		}
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	retain();
	IOLockLock(gSurfaceLock);
	ref = findRef(right->surface->sid, false);
	known = ref != NULL;
	if (ref == NULL) {
		ref = addRef(right->surface, &known);
	}
	ret = ref ? returnSurface(ref, known, args) : kIOReturnNoMemory;
	IOLockUnlock(gSurfaceLock);
	release();
	if (right->surface->traced) {
		char procname[32] = "?";
		uint32_t format = right->surface->info.pixelFormat, nonzero = 0, first = 0, middle = 0;
		const uint8_t * pixels = right->surface->kpixels;
		uint32_t size = right->surface->info.allocSize;

		/* what is in it by now: how many of its bytes are not zero, its first pixel and one in the middle */
		for (uint32_t at = 0; pixels != NULL && at < size; at += 64) {
			nonzero += pixels[at] != 0 || pixels[at + 1] != 0 || pixels[at + 2] != 0 || pixels[at + 3] != 0;
		}
		if (pixels != NULL && size >= 8) {
			memcpy(&first, pixels, 4);
			memcpy(&middle, pixels + (size / 2 & ~3u), 4);
		}
		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] looked up surface %u (%ux%u '%c%c%c%c') from a port: %#x; of every 64 bytes %u of %u "
		    "begin with a pixel that is not 0, the first %#010x, one in the middle %#010x\n", procname, proc_selfpid(),
		    right->surface->sid, right->surface->info.width, right->surface->info.height, (int) (format >> 24 & 0xff),
		    (int) (format >> 16 & 0xff), (int) (format >> 8 & 0xff), (int) (format & 0xff), ret, nonzero, size / 64, first, middle);
	}
	obj->release();
	return ret;
}

mach_port_name_t
IOSurfaceRootUserClient::makeSendRight(task_t task, MiniIOSurface * s)
{
	IOSurfaceSendRight * right;
	mach_port_name_t name = 0;

	right = OSTypeAlloc(IOSurfaceSendRight);
	if (right == NULL || !right->init()) {
		OSSafeReleaseNULL(right);
		return 0;
	}
	IOLockLock(gSurfaceLock);
	surfaceHold(s);                         /* for as long as the send right exists */
	IOLockUnlock(gSurfaceLock);
	right->surface = s;
	copyPortNameForObjectInTask(task, right, &name);
	right->release();
	return name;
}

mach_port_name_t
MiniIOSurfaceMakeSendRight(task_t task, MiniIOSurface * surface)
{
	return IOSurfaceRootUserClient::makeSendRight(task, surface);
}

IOReturn
IOSurfaceRootUserClient::methodCreatePort(IOExternalMethodArguments * args)
{
	MiniIOSurface * s;
	mach_port_name_t name;

	if (args->scalarInputCount < 1 || args->scalarOutputCount < 1) {
		return kIOReturnBadArgument;
	}
	s = MiniIOSurfaceCopy((uint32_t) args->scalarInput[0]);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	name = makeSendRight(fTask, s);
	if (s->traced) {
		char procname[32] = "?";

		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] made a port for surface %u: %#x\n", procname, proc_selfpid(), s->sid, name);
	}
	MiniIOSurfaceDrop(s);
	args->scalarOutput[0] = name;
	args->scalarOutputCount = 1;
	return name != 0 ? kIOReturnSuccess : kIOReturnNoMemory;
}

/* Requests about values start with the surface number in 8 bytes; the reply to a change is the new seed. */
static IOReturn
returnSeed(IOExternalMethodArguments * args, uint32_t seed)
{
	if (args->structureOutput != NULL && args->structureOutputSize >= sizeof(seed)) {
		memcpy(args->structureOutput, &seed, sizeof(seed));
		args->structureOutputSize = sizeof(seed);
	}
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodSetValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, seed = 0;
	OSObject * obj;
	OSArray * array;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnBadArgument;

	if (inSize < 12) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	obj = unserialize(in + 8, inSize - 8);
	array = OSDynamicCast(OSArray, obj);
	if (array == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref == NULL) {
		ret = kIOReturnNotFound;
	} else if (ref->surface->values != NULL) {
		MiniIOSurface * s = ref->surface;
		OSString * key = OSDynamicCast(OSString, array->getObject(1));
		OSDictionary * all = OSDynamicCast(OSDictionary, array->getObject(0));

		if (array->getCount() == 2 && key != NULL) {
			s->values->setObject(key, array->getObject(0));         /* [value, key] */
			ret = kIOReturnSuccess;
		} else if (array->getCount() == 1 && all != NULL) {
			s->values->merge(all);                                   /* [all the values] */
			ret = kIOReturnSuccess;
		}
		if (ret == kIOReturnSuccess) {
			s->updateValuesState();
			seed = s->shared()->valuesSeed;
		}
	}
	IOLockUnlock(gSurfaceLock);
	obj->release();
	return ret == kIOReturnSuccess ? returnSeed(args, seed) : ret;
}

IOReturn
IOSurfaceRootUserClient::methodRemoveValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, seed = 0;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (inSize < 9 || in[inSize - 1] != 0) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL && ref->surface->values != NULL) {
		MiniIOSurface * s = ref->surface;
		const char * key = (const char *) in + 8;

		if (key[0] == 0) {
			s->values->flushCollection();
		} else {
			s->values->removeObject(key);
		}
		s->updateValuesState();
		seed = s->shared()->valuesSeed;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret == kIOReturnSuccess ? returnSeed(args, seed) : ret;
}

/* The reply: 4 bytes unused, the seed, then the serialized value (a dictionary when all were asked for). */
IOReturn
IOSurfaceRootUserClient::methodCopyValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, header[2] = { 0, 0 };
	struct minisurf_client_ref * ref;
	OSSerialize * s = NULL;
	IOReturn ret = kIOReturnNotFound;
	uint32_t length = 0, total;

	if (inSize < 9 || in[inSize - 1] != 0) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL && ref->surface->values != NULL) {
		const char * key = (const char *) in + 8;
		OSObject * value = key[0] ? ref->surface->values->getObject(key) : ref->surface->values;

		header[1] = ref->surface->shared()->valuesSeed;
		ret = kIOReturnSuccess;
		if (value != NULL && (key[0] != 0 || ref->surface->values->getCount() != 0)) {
			s = OSSerialize::binaryWithCapacity(4096);
			if (s == NULL || !value->serialize(s)) {
				ret = kIOReturnNoMemory;
			} else {
				length = s->getLength();
			}
		}
	}
	IOLockUnlock(gSurfaceLock);

	total = (uint32_t) sizeof(header) + length;
	if (ret == kIOReturnSuccess) {
		if (args->structureOutputDescriptor != NULL) {
			IOMemoryDescriptor * out = args->structureOutputDescriptor;

			if (out->getLength() < total) {
				ret = kIOReturnNoSpace;
			} else if ((ret = out->prepare()) == kIOReturnSuccess) {
				out->writeBytes(0, header, sizeof(header));
				if (length != 0) {
					out->writeBytes(sizeof(header), s->text(), length);
				}
				out->complete();
				args->structureOutputDescriptorSize = total;
			}
		} else if (args->structureOutput == NULL || args->structureOutputSize < total) {
			ret = kIOReturnNoSpace;
		} else {
			memcpy(args->structureOutput, header, sizeof(header));
			if (length != 0) {
				memcpy((uint8_t *) args->structureOutput + sizeof(header), s->text(), length);
			}
			args->structureOutputSize = total;
		}
	}
	OSSafeReleaseNULL(s);
	return ret;
}

/*
 * Colour space, transfer function and the like: a fixed structure kept in the shared page.
 * The request: the structure, its size, a mask of the fields that changed, the surface number.
 */
IOReturn
IOSurfaceRootUserClient::methodSetBulkAttachments(const uint8_t * in, size_t inSize)
{
	static const struct {
		uint8_t from, to;
	} fields[15] = {
		{ 0x00, 0x20 }, { 0x20, 0x30 }, { 0x30, 0x38 }, { 0x38, 0x39 }, { 0x39, 0x3a }, { 0x3a, 0x3b }, { 0x3b, 0x3c },
		{ 0x3c, 0x3d }, { 0x3d, 0x3e }, { 0x3e, 0x3f }, { 0x3f, 0x40 }, { 0x40, 0x58 }, { 0x58, 0x5c }, { 0x5c, 0x64 },
		{ 0x64, 0x65 },
	};
	uint64_t size, mask;
	uint32_t sid;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (inSize < 0x88) {
		return kIOReturnBadArgument;
	}
	memcpy(&size, in + 0x70, sizeof(size));
	memcpy(&mask, in + 0x78, sizeof(mask));
	memcpy(&sid, in + 0x80, sizeof(sid));
	if (size > 0x70) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		uint8_t * attachments = ref->surface->shared()->attachments;

		for (unsigned i = 0; i < 15; i++) {
			if ((mask & (1ULL << i)) && fields[i].to <= size) {
				memcpy(attachments + fields[i].from, in + fields[i].from, fields[i].to - fields[i].from);
			}
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint8_t * in = (const uint8_t *) args->structureInput;
	size_t inSize = args->structureInputSize;
	IOMemoryMap * inMap = NULL;
	struct minisurf_client_ref * ref;
	IOReturn ret;

	switch (selector) {
	case kSurfaceMethodInit:
		return methodInit(args);
	case kSurfaceMethodCreate:
		return methodCreate(args);
	case kSurfaceMethodLookup:
		return methodLookup(args);
	case kSurfaceMethodLookupFromPort:
		return methodLookupFromPort(args);
	case kSurfaceMethodCreatePort:
		return methodCreatePort(args);

	case kSurfaceMethodSetValue:
	case kSurfaceMethodCopyValue:
	case kSurfaceMethodRemoveValue:
	case kSurfaceMethodSetBulkAttachments:
		if (args->structureInputDescriptor != NULL) {
			inMap = args->structureInputDescriptor->map(kIOMapReadOnly);
			if (inMap == NULL) {
				return kIOReturnNoMemory;
			}
			in = (const uint8_t *) inMap->getVirtualAddress();
			inSize = inMap->getLength();
		}
		if (in == NULL) {
			ret = kIOReturnBadArgument;
		} else if (selector == kSurfaceMethodSetValue) {
			ret = methodSetValue(args, in, inSize);
		} else if (selector == kSurfaceMethodCopyValue) {
			ret = methodCopyValue(args, in, inSize);
		} else if (selector == kSurfaceMethodRemoveValue) {
			ret = methodRemoveValue(args, in, inSize);
		} else {
			ret = methodSetBulkAttachments(in, inSize);
		}
		OSSafeReleaseNULL(inMap);
		return ret;

	case kSurfaceMethodSetPurgeable:                /* in: surface, new state; out: old state */
		if (args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		ret = kIOReturnNotFound;
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			if (args->scalarOutputCount >= 1) {
				args->scalarOutput[0] = ref->surface->purgeable;
				args->scalarOutputCount = 1;
			}
			if (args->scalarInput[1] != 3) {        /* 3 = keep current */
				ref->surface->purgeable = (uint32_t) args->scalarInput[1];
			}
			ret = kIOReturnSuccess;
		}
		IOLockUnlock(gSurfaceLock);
		return ret;

	case kSurfaceMethodBindAccel:                   /* in: surface, accelerator, sub-id */
		if (args->scalarInputCount < 3) {
			return kIOReturnBadArgument;
		}
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			ref->surface->shared()->accelID = (uint16_t) args->scalarInput[1];
			ref->surface->shared()->accelSubID = (uint16_t) args->scalarInput[2];
		}
		IOLockUnlock(gSurfaceLock);
		return ref ? kIOReturnSuccess : kIOReturnNotFound;

	case kSurfaceMethodSetYCbCrMatrix:              /* in: surface, matrix */
		if (args->scalarInputCount < 2) {
			break;
		}
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			ref->surface->shared()->ycbcrMatrix = (uint32_t) args->scalarInput[1];
		}
		IOLockUnlock(gSurfaceLock);
		return ref ? kIOReturnSuccess : kIOReturnNotFound;

	case kSurfaceMethodSetOwnership:
	case kSurfaceMethodSetOwnershipIdentity:
	case kSurfaceMethodSetTimestamp:
	case kSurfaceMethodSetIndexedTimestamp:
	case kSurfaceMethodDetachModeCode:
	case kSurfaceMethodLog:
		/* accounting and diagnostics: nothing depends on them here */
		if (args->scalarOutputCount != 0) {
			bzero(args->scalarOutput, args->scalarOutputCount * sizeof(args->scalarOutput[0]));
		}
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gSurfaceLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gSurfaceLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOSurface: selector %u not implemented (scalars %u in, %u out; structure %u in, %u out)\n", selector,
		    args->scalarInputCount, args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService ** targetP, UInt32 index)
{
	if (index >= kSurfaceTrapCount) {
		return NULL;
	}
	*targetP = this;
	return &fTraps[index];
}

IOReturn
IOSurfaceRootUserClient::useCountCommon(uint32_t sid, uint32_t category, int delta)
{
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (category > 1) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		MiniIOSurface * s = ref->surface;

		if (delta > 0 || ref->use[category] != 0) {
			ref->use[category] += delta;
			s->useCount[category] += delta;
			s->shared()->useCount = (uint64_t) s->useCount[1] << 32 | s->useCount[0];
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapIncrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return useCountCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, 1);
}

IOReturn
IOSurfaceRootUserClient::trapDecrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return useCountCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, -1);
}

/*
 * Legacy surfaces keep the seed-only lock contract. Owned Mali roots additionally
 * keep counted task CPU locks, and the actual UPL lease blocks escaped mappings
 * during GPU ownership; a Surface lock Boolean alone is not an alias barrier.
 */
IOReturn
IOSurfaceRootUserClient::lockCommon(uint32_t sid, uint32_t options, user_addr_t seedOut, bool unlock)
{
	struct minisurf_client_ref * ref;
	MiniIOSurface * held = NULL;
	uint64_t heldGeneration = 0;
	bool readOnly = (options & kSurfaceLockReadOnly) != 0;
	int acquired = LMAL_OK;
	IOReturn ret = kIOReturnSuccess;
	uint32_t seed = 0;

	retain();
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref == NULL || fClosing || current_task() != fTask) {
		IOLockUnlock(gSurfaceLock); release(); return kIOReturnNotFound;
	}
	if (!unlock && ref->surface->maliQueueFence != NULL) {
		held = ref->surface;
		heldGeneration = ref->maliGeneration;
		surfaceHold(held);
		IOLockUnlock(gSurfaceLock);
		bool completed = LancelotMaliQueueSurfaceWait(held, LMAL_MAX_WAIT_NS);
		IOLockLock(gSurfaceLock);
		ref = findRef(sid, false);
		if (!completed || fClosing || !ref || ref->surface != held || ref->maliGeneration != heldGeneration) {
			surfaceUnhold(held);
			IOLockUnlock(gSurfaceLock);release();
			return completed ? kIOReturnNotReady : kIOReturnTimeout;
		}
		surfaceUnhold(held);held = NULL;
	}
	if (ref->surface->maliCpuLease != NULL) {
		uint32_t *count = readOnly ? &ref->maliReads : &ref->maliWrites;
		uint32_t other = readOnly ? ref->maliWrites : ref->maliReads;
		if (ref->maliPending || other || (unlock && !*count) || (!unlock && *count == UINT32_MAX)) {
			IOLockUnlock(gSurfaceLock); release(); return kIOReturnBusy;
		}
		if (unlock) {
			if (--*count == 0) {
				ref->surface->maliCpuLease->endUserCpu(fTask, !readOnly);
			}
		} else if (*count) {
			++*count;
		} else {
			/* Never wait holding the global surface lock: the producer's
			 * unlock/close and final GPU retirement must remain runnable. */
			ref->maliPending = true;
			held = ref->surface;
			heldGeneration = ref->maliGeneration;
			surfaceHold(held);
			IOLockUnlock(gSurfaceLock);
			acquired = held->maliCpuLease->beginUserCpu(fTask, !readOnly, LMAL_MAX_WAIT_NS);
			IOLockLock(gSurfaceLock);
			ref = findRef(sid, false);
			if (fClosing || !ref || ref->surface != held || ref->maliGeneration != heldGeneration ||
			    !ref->maliPending || acquired) {
				if (!acquired) {
					held->maliCpuLease->endUserCpu(fTask, !readOnly);
				}
				/* Clear only the original ref (possibly detached/moved), never
				 * the pending flag of a newer lookup for this same surface. */
				for (uint32_t i = 0; i < fRefCount; ++i)
					if (fRefs[i].maliGeneration == heldGeneration) fRefs[i].maliPending = false;
				ret = acquired == LMAL_ETIMEDOUT ? kIOReturnTimeout : kIOReturnNotReady;
			} else {
				ref->maliPending = false;
				if (readOnly) ref->maliReads = 1;
				else ref->maliWrites = 1;
			}
		}
	}
	if (ret == kIOReturnSuccess) {
		struct minisurf_shared * sh = ref->surface->shared();

		if (!(options & kSurfaceLockReadOnly)) {
			sh->seed++;
		}
		seed = sh->seed;
	}
	if (held) surfaceUnhold(held);
	IOLockUnlock(gSurfaceLock);
	if (ret == kIOReturnSuccess && seedOut != 0) {
		copyout(&seed, seedOut, sizeof(seed));
	}
	release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return lockCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, (user_addr_t) (uintptr_t) p3, false);
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return lockCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, (user_addr_t) (uintptr_t) p3, true);
}

/*
 * A client drops a surface in two steps: "detach" while it still holds its own lock, and
 * "release" after it. A lookup of the same surface in between must make a fresh view.
 */
IOReturn
IOSurfaceRootUserClient::trapDetach(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	struct minisurf_client_ref * ref;

	IOLockLock(gSurfaceLock);
	ref = findRef((uint32_t) (uintptr_t) p1, false);
	if (ref != NULL) {
		ref->detached = true;
	}
	IOLockUnlock(gSurfaceLock);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	struct minisurf_client_ref * ref;
	uint32_t sid = (uint32_t) (uintptr_t) p1;

	IOLockLock(gSurfaceLock);
	ref = findRef(sid, true);
	if (ref == NULL) {
		ref = findRef(sid, false);
	}
	if (ref != NULL) {
		dropRef(ref);
	}
	IOLockUnlock(gSurfaceLock);
	return ref ? kIOReturnSuccess : kIOReturnNotFound;
}
