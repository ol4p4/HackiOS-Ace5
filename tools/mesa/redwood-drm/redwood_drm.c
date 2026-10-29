/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"

#define REDWOOD_GPU_SERVICE     "RedwoodGPU"
#define REDWOOD_GPU_SURFACE     0x7e            /* the request that is not Linux's */
#define REDWOOD_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */

/*
 * Vulkan's fences and semaphores are syncobjs, whose handles Turnip hands the kernel inside the
 * submit itself (MSM_SUBMIT_SYNCOBJ_IN/OUT). The kernel service knows none of those requests --
 * the GL driver never asks for one -- so the table lives here: a handle is a row of it, the
 * out-syncobjs of a submit are bound to the fence that submit was given (see redwood_submit
 * below), and a wait on one waits on that fence through WAIT_FENCE, the request the GL driver
 * waits on too. A row whose handle is 0 is one drmSyncobjDestroy has retired; handles never
 * come back, so a retired row is only skipped, and the table is compacted when it is half dead.
 */
struct redwood_syncobj {
	uint32_t handle;
	uint64_t point;                         /* the value it was last signaled with */
	uint64_t pending_point;                 /* what the fence still to come will signal it to */
	uint32_t fence;                         /* the submission that signals it; 0 while none */
	int signaled;
};

static struct redwood_syncobj *syncobjs;
static unsigned syncobjs_used, syncobjs_cap;
static uint32_t next_syncobj_handle = 1;        /* the kernel's fences count from 1 too; these are another row space */
static uint32_t next_fake_fence = 0x7f000000;   /* an empty submit's fence, of which the kernel sees nothing */

static struct redwood_syncobj *
syncobj_at(uint32_t handle)
{
	for (unsigned i = 0; i < syncobjs_used; i++) {
		if (syncobjs[i].handle == handle) {
			return &syncobjs[i];
		}
	}
	return NULL;
}

static struct redwood_syncobj *
syncobj_new(void)
{
	unsigned dead = 0;

	for (unsigned i = 0; i < syncobjs_used; i++) {
		if (syncobjs[i].handle == 0) {
			dead++;
		}
	}
	if (dead > syncobjs_used / 2) {         /* more of them retired than live: close the ranks */
		unsigned kept = 0;

		for (unsigned i = 0; i < syncobjs_used; i++) {
			if (syncobjs[i].handle != 0) {
				syncobjs[kept++] = syncobjs[i];
			}
		}
		syncobjs_used = kept;
	}
	if (syncobjs_used == syncobjs_cap) {
		unsigned cap = syncobjs_cap != 0 ? syncobjs_cap * 2 : 64;
		struct redwood_syncobj *grown = realloc(syncobjs, cap * sizeof(*grown));

		if (grown == NULL) {
			return NULL;
		}
		syncobjs = grown;
		syncobjs_cap = cap;
	}
	struct redwood_syncobj *s = &syncobjs[syncobjs_used++];

	*s = (struct redwood_syncobj) { .handle = next_syncobj_handle++ };
	return s;
}

static int64_t
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t) t.tv_sec * 1000000000 + t.tv_nsec;
}

/*
 * Waits for one syncobj to reach point. The deadline is an absolute time of CLOCK_MONOTONIC --
 * what Linux's syncobj ioctl takes, and what vk_drm_syncobj.c passes as the product of
 * os_time_get_absolute_timeout(). Answers 0 when it is reached, 1 when the deadline passed
 * first (and the caller's errno says ETIME, which is what Linux's says), -1 for the rest.
 */
static int
syncobj_wait_one(int fd, struct redwood_syncobj *s, uint64_t point, int64_t deadline_ns)
{
	if (s->signaled && (point == 0 || s->point >= point)) {
		return 0;
	}
	while (s->fence != 0) {
		struct drm_msm_wait_fence wf = {
			.fence = s->fence,
			.timeout = { .tv_sec = deadline_ns / 1000000000, .tv_nsec = deadline_ns % 1000000000 },
		};
		int ret = drmCommandWrite(fd, DRM_MSM_WAIT_FENCE, &wf, sizeof(wf));

		if (ret == 0) {
			s->signaled = 1;
			/* a binary syncobj (no point of its own) is done at its fence's point 1 */
			s->point = s->pending_point != 0 ? s->pending_point : 1;
			return (point == 0 || s->point >= point) ? 0 : 1;
		}
		if (ret == ETIMEDOUT || ret == -ETIMEDOUT) {
			/* the kernel's patience in a wait is capped (GPU_PATIENCE_MS), so a timeout
			   before the deadline is only reason to wait again, not to give up */
			if (now_ns() < deadline_ns) {
				continue;
			}
			errno = ETIME;
			return 1;
		}
		errno = EIO;
		return -1;
	}
	/*
	 * No fence to wait on the kernel for: either the submit that will signal it has not run
	 * yet -- WAIT_FOR_SUBMIT is the caller's promise that one is coming -- or nothing ever
	 * will. Either way the answer can only be looked for here, until the deadline.
	 */
	for (;;) {
		if (s->signaled && (point == 0 || s->point >= point)) {
			return 0;
		}
		if (s->fence != 0) {
			return syncobj_wait_one(fd, s, point, deadline_ns);
		}
		if (now_ns() >= deadline_ns) {
			errno = ETIME;
			return 1;
		}
		usleep(1000);
	}
}

/* 0 when every point of every syncobj was reached, -1 with errno set otherwise; the wait runs
   to the deadline for each in turn, so a WAIT_ALL of many is as patient as its slowest. */
static int
syncobj_wait(int fd, uint32_t *handles, const uint64_t *points, uint32_t count, int64_t deadline_ns,
    uint32_t flags, uint32_t *first_signaled)
{
	uint32_t done = 0;

	for (uint32_t i = 0; i < count; i++) {
		struct redwood_syncobj *s = syncobj_at(handles[i]);
		uint64_t point = points != NULL ? points[i] : 0;
		int waited;

		if (s == NULL) {
			errno = EINVAL;
			return -1;
		}
		waited = syncobj_wait_one(fd, s, point, deadline_ns);
		if (waited < 0) {
			return -1;
		}
		if (waited == 0) {
			if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) == 0) {
				if (first_signaled != NULL) {
					*first_signaled = i;
				}
				return 0;
			}
			done++;
		} else if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) == 0) {
			errno = ETIME;
			return -1;
		}
	}
	if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0 && done < count) {
		errno = ETIME;
		return -1;
	}
	return 0;
}

static io_connect_t connection;
static int connection_fd = -1;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0, IOServiceMatching(REDWOOD_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

static int redwood_submit(int fd, struct drm_msm_gem_submit *req, unsigned selector);

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (request == DRM_IOCTL_MSM_GEM_SUBMIT) {
		return redwood_submit(fd, arg, (unsigned)(request & 0xff));
	}
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	if (index == DRM_MSM_GEM_SUBMIT && size == sizeof(struct drm_msm_gem_submit)) {
		return redwood_submit(fd, (void *)(uintptr_t)data, (unsigned)(DRM_COMMAND_BASE + index));
	}
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	if (index == DRM_MSM_GEM_SUBMIT && size == sizeof(struct drm_msm_gem_submit)) {
		return redwood_submit(fd, data, (unsigned)(DRM_COMMAND_BASE + index));
	}
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 7;
		version->name = strdup("msm");
		version->name_len = 3;
		version->date = strdup("");
		version->desc = strdup("Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

void *
redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
redwood_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = REDWOOD_GPU_DESCRIBE;
	error = call(REDWOOD_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct redwood_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(REDWOOD_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	struct redwood_syncobj *s = syncobj_new();

	(void)fd;
	if (s == NULL) {
		errno = ENOMEM;
		return -1;
	}
	s->signaled = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
	s->point = s->signaled ? 1 : 0;         /* a signaled syncobj has a point, binary's is 1 */
	*handle = s->handle;
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct redwood_syncobj *s = syncobj_at(handle);

	(void)fd;
	if (s != NULL) {
		s->handle = 0;
	}
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	for (uint32_t i = 0; i < count; i++) {
		struct redwood_syncobj *s = syncobj_at(handles[i]);

		if (s == NULL) {
			errno = EINVAL;
			return -1;
		}
		s->signaled = 0;
		s->point = 0;
		s->pending_point = 0;
		s->fence = 0;
	}
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	for (uint32_t i = 0; i < count; i++) {
		struct redwood_syncobj *s = syncobj_at(handles[i]);

		if (s == NULL) {
			errno = EINVAL;
			return -1;
		}
		s->signaled = 1;
		if (s->point == 0) {
			s->point = 1;
		}
		s->fence = 0;                   /* a signaled syncobj waits for no submission */
	}
	return 0;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, const uint64_t *points, uint32_t count)
{
	(void)fd;
	for (uint32_t i = 0; i < count; i++) {
		struct redwood_syncobj *s = syncobj_at(handles[i]);

		if (s == NULL) {
			errno = EINVAL;
			return -1;
		}
		if (points[i] > s->point) {
			s->point = points[i];
		}
		s->pending_point = s->point;
		s->signaled = 1;
		s->fence = 0;
	}
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	(void)fd;
	for (uint32_t i = 0; i < count; i++) {
		struct redwood_syncobj *s = syncobj_at(handles[i]);

		if (s == NULL) {
			errno = EINVAL;
			return -1;
		}
		points[i] = s->signaled ? (s->point != 0 ? s->point : 1) : 0;
	}
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, uint32_t count, int64_t timeout_ns, uint32_t flags,
    uint32_t *first_signaled)
{
	return syncobj_wait(fd, handles, NULL, count, timeout_ns, flags, first_signaled);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, const uint64_t *points, uint32_t count,
    int64_t timeout_ns, uint32_t flags, uint32_t *first_signaled)
{
	return syncobj_wait(fd, handles, points, count, timeout_ns, flags, first_signaled);
}

/*
 * A submit with its syncobjs. Turnip's every submit carries its waits and signals in the
 * request itself (MSM_SUBMIT_SYNCOBJ_IN/OUT); the kernel service takes neither. The waits are
 * waited here, before anything of the submit happens -- Linux's kernel would take them without
 * a deadline of their own, so a generous one stands in, that a dependency nobody will satisfy
 * be an error rather than a wedge. The signals are bound to the fence the submit is given:
 * waiting on one waits for the GPU to really be through with what was submitted. A submit with
 * nothing for the GPU in it (nr_cmds 0, only fences and semaphores) the kernel service refuses;
 * the GPU has nothing to wait for in it either, so its fence is given out here and its
 * out-syncobjs signal at once.
 */
static int
redwood_submit(int fd, struct drm_msm_gem_submit *req, unsigned selector)
{
	uint32_t stride = req->syncobj_stride != 0 ? req->syncobj_stride
	    : sizeof(struct drm_msm_gem_submit_syncobj);
	int64_t deadline = now_ns() + 60 * 1000000000ll;
	int error;

	for (uint32_t i = 0; i < req->nr_in_syncobjs; i++) {
		struct drm_msm_gem_submit_syncobj *w =
		    (void *)(uintptr_t)(req->in_syncobjs + (uint64_t) i * stride);
		struct redwood_syncobj *s = syncobj_at(w->handle);
		int waited;

		if (s == NULL) {
			continue;
		}
		waited = syncobj_wait_one(fd, s, w->point, deadline);
		if (waited < 0) {
			return -1;
		}
		if (waited > 0) {
			errno = ETIMEDOUT;
			return -1;
		}
	}
	if (req->nr_cmds == 0 && req->nr_bos == 0) {
		req->fence = next_fake_fence++;
		for (uint32_t i = 0; i < req->nr_out_syncobjs; i++) {
			struct drm_msm_gem_submit_syncobj *sg =
			    (void *)(uintptr_t)(req->out_syncobjs + (uint64_t) i * stride);
			struct redwood_syncobj *s = syncobj_at(sg->handle);

			if (s == NULL) {
				continue;
			}
			if (sg->point > s->point) {
				s->point = sg->point;
			}
			if (s->point == 0) {
				s->point = 1;
			}
			s->signaled = 1;
		}
		return 0;
	}
	error = call(selector, req, sizeof(*req));
	if (error != 0) {
		errno = error;
		return -1;
	}
	for (uint32_t i = 0; i < req->nr_out_syncobjs; i++) {
		struct drm_msm_gem_submit_syncobj *sg =
		    (void *)(uintptr_t)(req->out_syncobjs + (uint64_t) i * stride);
		struct redwood_syncobj *s = syncobj_at(sg->handle);

		if (s == NULL) {
			continue;
		}
		if (sg->point > s->pending_point) {
			s->pending_point = sg->point;
		}
		s->fence = req->fence;
	}
	return 0;
}

/*
 * The one device of the enumeration (see xf86drm.h): its nodes name the node drmOpenWithType()
 * opens, which both open() and stat() accept. The strings are static and outlive any caller.
 */
static char *redwood_nodes[DRM_NODE_MAX] = { "/dev/null", "/dev/null", "/dev/null" };
static drmDevice redwood_device = {
	.nodes = redwood_nodes,
	.available_nodes = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER),
};

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (devices == NULL || max_devices < 1) {
		return -EINVAL;
	}
	devices[0] = &redwood_device;
	return 1;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	(void)devices;
	(void)count;                    /* nothing was made per device, so nothing to take back */
}

/* The same one device, made the caller's (drmFreeDevice() frees it): what wsi_common_drm.c asks
   when it wants to know whether a device descriptor is this device. */
int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	drmDevicePtr made = malloc(sizeof(*made));

	(void)fd;
	(void)flags;
	if (made == NULL) {
		errno = ENOMEM;
		return -1;
	}
	*made = redwood_device;
	*device = made;
	return 0;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL && *device != NULL) {
		free(*device);
		*device = NULL;
	}
}

bool
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	if (a == NULL || b == NULL || a->available_nodes != b->available_nodes) {
		return false;
	}
	for (int node = 0; node < DRM_NODE_MAX; node++) {
		const char *one = (a->available_nodes & (1 << node)) ? a->nodes[node] : NULL;
		const char *other = (b->available_nodes & (1 << node)) ? b->nodes[node] : NULL;

		if ((one == NULL) != (other == NULL) || (one != NULL && strcmp(one, other) != 0)) {
			return false;
		}
	}
	return true;
}

/* Linux moves a timeline point from one syncobj to another; there is no timeline here
   (drmGetCap() says so) and nothing asks. The table can still say what a transfer means:
   the source's state becomes the destination's, at the point asked of it. */
int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags)
{
	struct redwood_syncobj *dst = syncobj_at(dst_handle);
	struct redwood_syncobj *src = syncobj_at(src_handle);

	(void)fd;
	(void)flags;
	if (dst == NULL || src == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (src->signaled && (src_point == 0 || src->point >= src_point)) {
		dst->signaled = 1;
		dst->point = dst_point != 0 ? dst_point : src->point;
		dst->fence = 0;
	}
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjImportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int sync_file_fd)
NONE(drmSyncobjExportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *sync_file_fd)
