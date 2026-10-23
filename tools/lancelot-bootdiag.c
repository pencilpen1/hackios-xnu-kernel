/* Lightweight boot observer, linked with userland/libmini; never halts the OS. */
#include "mini.h"

/* Public PROC_PIDTBSDINFO ABI of the pinned XNU. No private process pointers. */
struct proc_bsdinfo {
	uint32_t flags, status, xstatus, pid, ppid, uid, gid, ruid, rgid, svuid, svgid, reserved;
	char comm[16], name[32];
	uint32_t nfiles, pgid, jobc, tdev, tpgid;
	int32_t nice;
	uint64_t start_sec, start_usec;
};
_Static_assert(sizeof(struct proc_bsdinfo) == 136, "PROC_PIDTBSDINFO ABI");
static const char *wanted[] = {"launchd", "SpringBoard", "backboardd", "runningboardd", "PosterBoard", "mediaserverd", "lsd", "wifid", "coredatad", "mobileactivationd"};
static const char *logfiles[] = {
	"/private/var/log/hackios/SpringBoard.log", "/private/var/log/hackios/backboardd.log",
	"/private/var/log/hackios/mediaserverd.log", "/private/var/log/hackios/lsd.log",
	"/private/var/log/hackios/lsd-system.log", "/private/var/log/hackios/wifid.log",
	"/private/var/log/hackios/coredatad.log",
};
#define LOG_COUNT (sizeof(logfiles) / sizeof(logfiles[0]))
static uint64_t metric(const char *name)
{
	char path[128];
	uint64_t value = 0;
	size_t size = sizeof(value);
	strcpy(path, "kern.redwood."); strcpy(path + strlen(path), name);
	if (sysctlbyname(path, &value, &size, NULL, 0) || size != sizeof(value)) return UINT64_MAX;
	return value;
}
static void processes(void)
{
	static int pids[2048];
	unsigned seen[sizeof(wanted) / sizeof(wanted[0])] = {0};
	long bytes = mini_syscall(336, 1, 1, 0, 0, (long)pids, sizeof(pids));
	if (bytes < 0 || (unsigned long)bytes > sizeof(pids)) { printf("bootdiag: proc list errno=%d\n", errno); return; }
	for (long i = 0; i < bytes / (long)sizeof(int); i++) {
		struct proc_bsdinfo info;
		long got = mini_syscall(336, 2, pids[i], 3, 0, (long)&info, sizeof(info));
		if (got != (long)sizeof(info)) continue;
		char name[33];
		memcpy(name, info.name[0] ? info.name : info.comm, info.name[0] ? 32 : 16);
		name[info.name[0] ? 32 : 16] = 0;
		for (unsigned j = 0; j < sizeof(wanted) / sizeof(wanted[0]); j++) {
			if (strcmp(name, wanted[j])) continue;
			seen[j]++;
			printf("bootdiag: %s pid=%u parent=%u uid=%u state=%u exit=%u start=%llu.%06llu\n", name, info.pid,
			    info.ppid, info.uid, info.status, info.xstatus, (unsigned long long)info.start_sec,
			    (unsigned long long)info.start_usec);
		}
	}
	for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++)
		if (!seen[i]) printf("bootdiag: %s not currently running\n", wanted[i]);
}
static int contains(const char *line, const char *word)
{
	for (; *line; line++) {
		const char *a = line, *b = word;
		while (*a && *b) {
			char c = *a >= 'A' && *a <= 'Z' ? *a + ('a' - 'A') : *a;
			if (c != *b) break;
			a++; b++;
		}
		if (!*b) return 1;
	}
	return 0;
}
static void tail(const char *path, off_t *cursor, off_t *forwarded)
{
	static char buffer[32769];
	struct stat st;
	int fd = open(path, O_RDWR, 0);
	if (fd < 0) return;
	if (fstat(fd, &st) || st.st_size < 0) { close(fd); return; }
	if (*cursor > st.st_size) *cursor = 0;
	off_t start = *cursor;
	if (st.st_size - start > (off_t)sizeof(buffer) - 1) start = st.st_size - (off_t)sizeof(buffer) + 1;
	ssize_t n = pread(fd, buffer, sizeof(buffer) - 1, start);
	if (n > 0 && (size_t)n < sizeof(buffer)) {
		buffer[n] = 0; *cursor = start + n;
		unsigned shown = 0;
		char *p = buffer;
		if (start && (p = strchr(p, '\n'))) p++;
		while (p && *p && shown < 10) {
			char *end = strchr(p, '\n');
			if (end) *end = 0;
			if (contains(p, "error") || contains(p, "failed") || contains(p, "migration") ||
			    contains(p, "left alone") || contains(p, "incomplete") || contains(p, "ready")) {
				/* Bound framebuffer work even if an os_log line is enormous. */
				if (strlen(p) > 240) p[240] = 0;
				printf("bootdiag: %s: %s\n", path, p); shown++;
			}
			p = end ? end + 1 : NULL;
		}
	}
	/* Dedicated volatile diagnostic logs only. Daemons retain their open FDs. */
	if (st.st_size > 4 * 1024 * 1024) {
		printf("bootdiag: log cap %s size=%lld unread=%lld\n", path, st.st_size,
		    st.st_size > *forwarded ? st.st_size - *forwarded : 0LL);
		if (!ftruncate(fd, 0)) { *cursor = 0; *forwarded = 0; }
	}
	close(fd);
}

/* Incremental, unfiltered file forwarding into the USB log ring, not the
 * framebuffer. Offsets and loss notices make truncation/backlog explicit. */
static void forward(const char *path, off_t *cursor)
{
	static char data[16384];
	struct stat st;
	int fd = open(path, O_RDONLY, 0);
	if (fd < 0) return;
	if (fstat(fd, &st) || st.st_size < 0) { close(fd); return; }
	if (*cursor > st.st_size) { printf("bootdiag: file truncated %s old_offset=%lld\n", path, *cursor); *cursor = 0; }
	ssize_t n = pread(fd, data, sizeof(data), *cursor);
	if (n > 0 && (size_t)n <= sizeof(data)) {
		/* This marker also identifies chunks that end halfway through a line. */
		printf("bootdiag: file-chunk %s offset=%lld bytes=%lld\n", path, *cursor, (long long)n);
		if (!sysctlbyname("kern.redwood.usb_log", NULL, NULL, data, (size_t)n)) *cursor += n;
	}
	close(fd);
}
int main(int argc, char **argv, char **envp)
{
	(void)argc; (void)argv; (void)envp;
	int fd = open("/dev/console", O_WRONLY, 0);
	if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
	off_t summaries[LOG_COUNT] = {0}, forwarded[LOG_COUNT] = {0};
	struct stat st;
	if (!stat("/private/var/log/hackios", &st))
		printf("bootdiag: log directory uid=%u gid=%u mode=%o\n", st.st_uid, st.st_gid, st.st_mode);
	if (!stat("/usr/libexec/backboardd", &st))
		printf("bootdiag: backboardd executable uid=%u gid=%u mode=%o\n", st.st_uid, st.st_gid, st.st_mode);
	for (unsigned sample = 0;; sample++) {
		struct timeval now;
		gettimeofday(&now, NULL);
		long next = now.tv_sec + 30;
		printf("bootdiag: sample=%u mmc width=%llu hz=%llu batch=%llu errors=%llu\n", sample,
		    (unsigned long long)metric("lancelot_mmc_bus_width"), (unsigned long long)metric("lancelot_mmc_clock_hz"),
		    (unsigned long long)metric("lancelot_mmc_batch_blocks"), (unsigned long long)metric("lancelot_mmc_errors"));
		printf("bootdiag: sectors=%llu commands=%llu pio_us=%llu fallback=%llu\n",
		    (unsigned long long)metric("lancelot_mmc_sectors"), (unsigned long long)metric("lancelot_mmc_commands"),
		    (unsigned long long)metric("lancelot_mmc_pio_us"), (unsigned long long)metric("lancelot_mmc_mode_fallbacks"));
		printf("bootdiag: usb polls=%llu resets=%llu commands=%llu sent=%llu\n",
		    (unsigned long long)metric("usb_log_polls"), (unsigned long long)metric("usb_log_resets"),
		    (unsigned long long)metric("usb_log_commands"), (unsigned long long)metric("usb_log_sent"));
		processes();
		for (unsigned i = 0; i < LOG_COUNT; i++) tail(logfiles[i], &summaries[i], &forwarded[i]);
		do {
			for (unsigned i = 0; i < LOG_COUNT; i++) forward(logfiles[i], &forwarded[i]);
			usleep(100000);
			gettimeofday(&now, NULL);
		} while (now.tv_sec < next);
	}
}
