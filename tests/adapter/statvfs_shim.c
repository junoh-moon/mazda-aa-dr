/* Test-environment shim, never shipped. glibc's statvfs() reads /proc/mounts and
 * stat()s every mount point to fill f_flag. The runtime-assist DSO suite runs the
 * ARM product under qemu-user, which exposes the HOST mount table; on a host with
 * hundreds of mounts each product storage check then costs milliseconds and the
 * authored 80-250 ms windows starve. The CMU table has about a dozen entries.
 * This replaces only that libc call, and only when run_unwind_dso.py finds a large
 * host table. The product code is unchanged and still decides what to do with the
 * result; statfs() failures are reported as statvfs() failures. */
#include <string.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>

int statvfs(const char* path, struct statvfs* v) {
    struct statfs f;
    if (statfs(path, &f)) return -1;
    memset(v, 0, sizeof *v);
    v->f_bsize = f.f_bsize;
    v->f_frsize = f.f_bsize ? f.f_bsize : 4096;
    v->f_blocks = f.f_blocks;
    v->f_bfree = f.f_bfree;
    v->f_bavail = f.f_bavail;
    v->f_files = f.f_files;
    v->f_ffree = f.f_ffree;
    v->f_favail = f.f_ffree;
    v->f_namemax = f.f_namelen;
    return 0;
}
