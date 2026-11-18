#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sm18635_protocol.h"

static unsigned writes;
static int mode(const char *value)
{
    const char *m = getenv("GPIO_MOCK_MODE");
    return m && !strcmp(m, value);
}
static void require(int condition)
{
    if (!condition) { fprintf(stderr, "GPIO mock contract violation\n"); abort(); }
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    require(fd == 123);
    if (request == SM18635_GET_ABI)
        return mode("abi") ? 1 : SM18635_ABI_VERSION;
    require(request == SM18635_GET_GPIO);
    if (mode("gpio-query")) { errno = ENOTTY; return -1; }
    if (mode("gpio-invalid")) return 96;
    return getenv("GPIO_MOCK_PIN") ? atoi(getenv("GPIO_MOCK_PIN")) : (int)SM18635_GPIO;
}

int __wrap_open(const char *name, int flags, ...)
{
    require(!strcmp(name, "/dev/sm18635_gpio") || !strcmp(name, "/dev/mockgpio"));
    require((flags & O_ACCMODE) == O_WRONLY);
    if (mode("open")) { errno = ENOENT; return -1; }
    return 123;
}

ssize_t __wrap_write(int fd, const void *data, size_t length)
{
    struct sm18635_request request, bad;
    struct sm18635_pulse pulses[SM18635_MAX_PULSES];
    unsigned n, i, total = 0;
    require(fd == 123 && length == 28 && sizeof(request) == 28 && ++writes == 1);
    memcpy(&request, data, sizeof(request));
    require(sm18635_valid(&request));
    bad = request; bad.version = SM18635_ABI_VERSION + 1; require(!sm18635_valid(&bad));
    bad = request; bad.chips = 0; require(!sm18635_valid(&bad));
    bad = request; bad.chips = 5; require(!sm18635_valid(&bad));
    bad = request; bad.gain[4] = 64; require(!sm18635_valid(&bad));
    bad = request; bad.reserved[1] = 1; require(!sm18635_valid(&bad));
    bad = request; bad.din_flip = 2; require(!sm18635_valid(&bad));
    require(sm18635_build_pulses(pulses, &bad) == 0);
    n = sm18635_build_pulses(pulses, &request);
    for (i = 0; i < n; ++i) total += pulses[i].high_ns + pulses[i].low_ns;
    printf("MOCK count=%u nominal_ns=%u\n", n, total);
    printf("MOCK pulses=");
    for (i = 0; i < n; ++i) printf("%u/%u,", pulses[i].high_ns, pulses[i].low_ns);
    putchar('\n');
    if (mode("short")) return length - 1;
    if (mode("io")) { errno = EIO; return -1; }
    if (mode("busy")) { errno = EBUSY; return -1; }
    if (mode("eintr")) { errno = EINTR; return -1; }
    if (mode("timeout")) { errno = ETIMEDOUT; return -1; }
    return length;
}

ssize_t __wrap_read(int fd, void *data, size_t length)
{
    (void)fd; (void)data; (void)length;
    require(0);
    return -1;
}

int __wrap_close(int fd)
{
    require(fd == 123 && writes ==
            ((mode("abi") || mode("gpio-query") || mode("gpio-invalid")) ? 0u : 1u));
    if (mode("close")) { errno = EIO; return -1; }
    return 0;
}
