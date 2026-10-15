/* ARM LD_PRELOAD probe for the offline bds parse check
 * (tests/packaging/test_bds_parse_qemu.py, validation/PHONE_IMU_CHANNEL_2026-10-14.md).
 * Our code, no OEM bytes; it never calls bds code. It interposes only
 * open()/open64() (forwarded unchanged) and, when bds opens its radio UART
 * (/dev/ttymxc3, after the configuration is loaded), reads two exported
 * globals of the stock bds: BDS_serialPortCount (1 byte) and
 * BDS_SerialPortList (160 bytes). Observed layout (DATA, 2026-10-14 run with
 * the stock XML as control): 20 entries of 8 bytes whose first word is the
 * serialPort id of the configuration, in file order. Reported in the file
 * named by MX5DR_BDS_PROBE:
 *   radio_open count=<n> ids=<id,id,...>
 * An earlier version also interposed BDS_SerialPortConfiguration_Add; its
 * argument layout is unknown and forwarding six words changed bds behaviour. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int out = -1;
static void say(const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (out >= 0 && n > 0) { ssize_t w = write(out, b, (size_t)n); (void)w; }
}
__attribute__((constructor)) static void init(void) {
    const char *p = getenv("MX5DR_BDS_PROBE");
    if (p) out = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    say("probe_loaded\n");
}
/* Readable for n bytes according to /proc/self/maps (qemu emulates it). */
static int readable(uintptr_t a, size_t n) {
    if (a < 4096) return 0;
    FILE *f = fopen("/proc/self/maps", "r"); char line[512]; int ok = 0;
    if (!f) return 0;
    while (!ok && fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) == 3 && perm[0] == 'r' && a >= lo && a + n <= hi) ok = 1;
    }
    fclose(f);
    return ok;
}
static void radio(const char *path) {
    if (!path || strcmp(path, "/dev/ttymxc3")) return;
    unsigned char *count = (unsigned char *)dlsym(RTLD_DEFAULT, "BDS_serialPortCount");
    uint32_t *list = (uint32_t *)dlsym(RTLD_DEFAULT, "BDS_SerialPortList");
    char ids[160] = ""; size_t k = 0;
    if (count && list && *count <= 20 && readable((uintptr_t)list, 160))
        for (int i = 0; i < *count && k + 12 < sizeof ids; ++i)
            k += (size_t)snprintf(ids + k, sizeof ids - k, "%s%lu", i ? "," : "", (unsigned long)list[2 * i]);
    say("radio_open count=%d ids=%s\n", count ? *count : -1, ids);
}
typedef int (*open_fn)(const char *, int, ...);
int open(const char *path, int flags, ...) {
    static open_fn real; mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");
    radio(path);
    return real(path, flags, mode);
}
int open64(const char *path, int flags, ...) {
    static open_fn real; mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open64");
    radio(path);
    return real(path, flags, mode);
}
