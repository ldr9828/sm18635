/* SM18635.c wire format, transported by Ingenic SFC1/SPI1 internal DMA via spidev. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

/* 128 zero bytes = 320 us even at the maximum 3.2 MHz SPI clock. */
#define RESET_BYTES 128u
#define MAX_CHIPS 32u
#define MAX_MESSAGE 4096u

struct config {
    unsigned chips;
    uint16_t gray[5];
    uint8_t gain[5];
    uint8_t control, din_flip, reg_control;
};

/* All fields are MSB first, including the six-bit current gains. */
static void encode(uint8_t *out, size_t *nibbles, unsigned value, unsigned bits)
{
    while (bits--) {
        uint8_t nibble = (value & (1u << bits)) ? 0xe : 0x8;
        size_t p = (*nibbles)++;
        out[p / 2] |= (uint8_t)(nibble << ((p & 1) ? 0 : 4));
    }
}

static size_t frame(uint8_t *out, const struct config *c)
{
    size_t start = RESET_BYTES + 5, n = 0;
    unsigned chip, ch;
    /* 32 SPI ones = 8 chip periods; eight zeros = 2 chip periods. */
    memset(out + RESET_BYTES, 0xff, 4);
    for (chip = 0; chip < c->chips; chip++) {
        encode(out + start, &n, 0x96, 8);
        for (ch = 0; ch < 5; ch++)
            encode(out + start, &n, c->gray[ch], 16);
        encode(out + start, &n, c->control, 8);
    }
    encode(out + start, &n, 0xe1, 8);
    for (ch = 0; ch < 5; ch++)
        encode(out + start, &n, c->gain[ch], 6);
    encode(out + start, &n, c->din_flip << 1, 2);
    encode(out + start, &n, c->reg_control, 8);
    return start + n / 2 + RESET_BYTES;
}

static unsigned number(const char *s, unsigned max)
{
    char *end;
    unsigned long v;
    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno || end == s || *end || *s == '-' || v > max) {
        fprintf(stderr, "Invalid number: %s (range 0..%u)\n", s, max);
        exit(EXIT_FAILURE);
    }
    return (unsigned)v;
}

static void five(const char *s, unsigned *values, unsigned max)
{
    char *copy = malloc(strlen(s) + 1), *p, *next;
    unsigned i;
    if (!copy) { perror("malloc"); exit(EXIT_FAILURE); }
    strcpy(copy, s);
    p = copy;
    for (i = 0; i < 5; i++) {
        next = strchr(p, ',');
        if ((i < 4 && !next) || (i == 4 && next)) {
            fprintf(stderr, "Expected exactly R,G,B,W,Y: %s\n", s);
            free(copy);
            exit(EXIT_FAILURE);
        }
        if (next) *next = '\0';
        values[i] = number(p, max);
        if (next) p = next + 1;
    }
    free(copy);
}

static void usage(const char *name)
{
    printf("Usage: %s [options]\n"
           "  -d DEVICE  spidev path (default /dev/spidev1.0)\n"
           "  -s HZ      SPI clock 1600000..3200000 (default 2000000)\n"
           "  -n COUNT   cascaded chips 1..32; same gray values for each\n"
           "  -g R,G,B,W,Y  gray values 0..65535 (default all 0)\n"
           "  -a R,G,B,W,Y  gains 0..63 (default 25,25,25,63,63)\n"
           "  -c BYTE    per-chip control byte (default 0)\n"
           "  -f BIT     DIN flip bit 0/1 (default 0)\n"
           "  -p BYTE    last register byte (default 0xc0, factory value)\n"
           "  -t         TX-only transfer (default; retained for compatibility)\n"
           "  -o FILE    write encoded binary only; do not open SPI\n"
           "Numbers accept decimal or 0x hex. Sends two identical frames\n"
           "in ONE TX-only SPI transfer for SFC1 internal DMA.\n", name);
}

int main(int argc, char **argv)
{
    struct config c = {1, {0,0,0,0,0}, {25,25,25,63,63}, 0, 0, 0xc0};
    const char *device = "/dev/spidev1.0", *dump = NULL;
    uint32_t speed = 2000000;
    uint8_t mode = SPI_MODE_0, bits = 8, lsb = 0;
    uint8_t tx[MAX_MESSAGE] = {0};
    size_t one, total;
    unsigned values[5], i;
    int opt, fd, ret;
    struct spi_ioc_transfer transfer = {0};

    while ((opt = getopt(argc, argv, "d:s:n:g:a:c:f:p:o:th")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 's': speed = number(optarg, 3200000); break;
        case 'n': c.chips = number(optarg, MAX_CHIPS); break;
        case 'g':
            five(optarg, values, 65535);
            for (i = 0; i < 5; i++) c.gray[i] = (uint16_t)values[i];
            break;
        case 'a':
            five(optarg, values, 63);
            for (i = 0; i < 5; i++) c.gain[i] = (uint8_t)values[i];
            break;
        case 'c': c.control = (uint8_t)number(optarg, 255); break;
        case 'f': c.din_flip = (uint8_t)number(optarg, 1); break;
        case 'p': c.reg_control = (uint8_t)number(optarg, 255); break;
        case 'o': dump = optarg; break;
        case 't': break; /* TX-only is the native SFC1 mode. */
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc || !c.chips || speed < 1600000) {
        usage(argv[0]); return EXIT_FAILURE;
    }
    one = frame(tx, &c);
    total = one * 2;
    if (total > sizeof(tx)) {
        fprintf(stderr, "Message exceeds spidev buffer\n"); return EXIT_FAILURE;
    }
    memcpy(tx + one, tx, one); /* first frame wakes chip; second applies data */
    printf("%u chip(s), %zu bytes/frame, %zu bytes/message, SPI %u Hz\n",
           c.chips, one, total, speed);
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (!f) { perror(dump); return EXIT_FAILURE; }
        ret = fwrite(tx, 1, total, f) == total ? 0 : -1;
        if (fclose(f)) ret = -1;
        if (ret) { fprintf(stderr, "Cannot write %s\n", dump); return EXIT_FAILURE; }
        return EXIT_SUCCESS;
    }
    fd = open(device, O_RDWR);
    if (fd < 0) { perror(device); return EXIT_FAILURE; }
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_LSB_FIRST, &lsb) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("SPI configuration"); close(fd); return EXIT_FAILURE;
    }
    /* SFC1 is half-duplex. A dummy RX buffer would be rejected by the
     * kernel; completion is signalled by the SFC END interrupt. */
    transfer.tx_buf = (uint64_t)(uintptr_t)tx;
    transfer.rx_buf = 0;
    transfer.len = (uint32_t)total;
    transfer.speed_hz = speed;
    transfer.bits_per_word = bits;
    ret = ioctl(fd, SPI_IOC_MESSAGE(1), &transfer);
    if (ret < 0) perror("SPI_IOC_MESSAGE");
    else if ((size_t)ret != total) fprintf(stderr, "Short transfer: %d\n", ret);
    close(fd);
    if (ret < 0 || (size_t)ret != total) return EXIT_FAILURE;
    puts("Transfer completed. DMA selection is controlled by the kernel/DTS;");
    puts("ioctl success alone does not prove DMA or waveform correctness.");
    return EXIT_SUCCESS;
}
