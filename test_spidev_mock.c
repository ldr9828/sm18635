/* Host-only contract test: emulate the SFC1 spidev ABI, no hardware access. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static unsigned step, messages;
static unsigned setting(const char *name, unsigned fallback)
{
    const char *value = getenv(name);
    return value ? (unsigned)strtoul(value, NULL, 10) : fallback;
}
int open(const char *path, int flags, ...)
{
    assert(strcmp(path, "/dev/spidev1.0") == 0);
    assert(flags == O_RDWR);
    return syscall(SYS_openat, AT_FDCWD, "/dev/null", O_RDWR, 0);
}
int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    (void)fd;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (step < 4) {
        switch (step++) {
        case 0:
            assert(request == SPI_IOC_WR_MODE && *(uint8_t *)arg == SPI_MODE_0);
            break;
        case 1:
            assert(request == SPI_IOC_WR_LSB_FIRST && *(uint8_t *)arg == 0);
            break;
        case 2:
            assert(request == SPI_IOC_WR_BITS_PER_WORD && *(uint8_t *)arg == 8);
            break;
        case 3:
            assert(request == SPI_IOC_WR_MAX_SPEED_HZ && *(uint32_t *)arg == 2000000);
            break;
        }
        return 0;
    }
    struct spi_ioc_transfer *t = arg;
    assert(request == SPI_IOC_MESSAGE(1));
    assert(t->tx_buf && !t->rx_buf && t->len == 666);
    assert(t->bits_per_word == 8 && t->speed_hz == 2000000 && !t->cs_change);
    assert(!memcmp((void *)(uintptr_t)t->tx_buf,
                   (void *)(uintptr_t)(t->tx_buf + 333), 333));
    messages++;
    if (messages == setting("MOCK_FAIL_AT", 0)) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (getenv("MOCK_SHORT_TRANSFER")) return t->len - 1;
    if (messages == setting("MOCK_SIGNAL_AT", 0)) {
        raise(getenv("MOCK_SIGTERM") ? SIGTERM : SIGINT);
    }
    return t->len;
}
__attribute__((destructor)) static void check_complete(void)
{
    assert(step == 4);
    assert(messages == setting("MOCK_EXPECT_MESSAGES", 1));
}
