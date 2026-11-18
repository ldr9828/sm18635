#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "sm18635_protocol.h"

static unsigned number(const char *s, unsigned max)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(s, &end, 0);
    if (errno || end == s || *end || *s == '-' || value > max) {
        fprintf(stderr, "Invalid number: %s (0..%u)\n", s, max);
        exit(EXIT_FAILURE);
    }
    return (unsigned)value;
}

static void five(const char *s, unsigned values[5], unsigned max)
{
    char *copy = strdup(s), *part, *next;
    unsigned i;
    if (!copy) { perror("strdup"); exit(EXIT_FAILURE); }
    part = copy;
    for (i = 0; i < 5; ++i) {
        next = strchr(part, ',');
        if ((i < 4 && !next) || (i == 4 && next)) {
            fprintf(stderr, "Expected exactly R,G,B,W,Y: %s\n", s);
            free(copy);
            exit(EXIT_FAILURE);
        }
        if (next) *next = '\0';
        values[i] = number(part, max);
        if (next) part = next + 1;
    }
    free(copy);
}

static void usage(const char *name)
{
    printf("Usage: %s [options]\n"
           "  -d DEVICE  driver node (default /dev/sm18635_gpio; NOT a tty)\n"
           "  -n COUNT   1..4 cascaded chips, identical gray per chip\n"
           "  -g R,G,B,W,Y  gray 0..65535 (default all zero)\n"
           "  -a R,G,B,W,Y  gain 0..63 (default 25,25,25,63,63)\n"
           "  -c BYTE    chip control (default 0)\n"
           "  -f BIT     chip DIN flip field (default 0; NOT GPIO inversion)\n"
           "  -p BYTE    register control (default 0xc0)\n"
           "  -o FILE    export original 2 MHz SPI reference; no device access\n"
           "  -h         help\n"
           "GPIO is selected by the module and queried at runtime.\n"
           "Idle LOW, positive pulses, TX-only, two frames then exit.\n"
           "GPIO software timing is nominal; scope validation is required.\n", name);
}

static int dump_reference(const char *path, const struct sm18635_request *r)
{
    struct sm18635_pulse pulses[SM18635_MAX_PULSES];
    unsigned count = sm18635_build_pulses(pulses, r), i;
    size_t one = 285u + 48u * r->chips, size = one * 2;
    unsigned char data[2u * (285u + 48u * SM18635_MAX_CHIPS)] = {0};
    FILE *f;
    int ret = 0;
    memset(data + 128, 0xff, 4);
    for (i = 1; i < count; ++i) {
        unsigned symbol = pulses[i].high_ns == 1500 ? 0xe : 0x8;
        unsigned bit = i - 1;
        data[133 + bit / 2] |= (unsigned char)(symbol << ((bit & 1) ? 0 : 4));
    }
    memcpy(data + one, data, one);
    f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (fwrite(data, 1, size, f) != size) ret = -1;
    if (fclose(f)) ret = -1;
    if (ret) fprintf(stderr, "Cannot write %s\n", path);
    else printf("SPI reference: %zu bytes/frame, %zu bytes/message\n", one, size);
    return ret;
}

int main(int argc, char **argv)
{
    struct sm18635_request r = {
        SM18635_ABI_VERSION, 1, {0,0,0,0,0}, {25,25,25,63,63}, 0, 0, 0xc0, {0,0}
    };
    const char *device = "/dev/sm18635_gpio", *dump = NULL;
    unsigned values[5], i;
    int opt, fd, ret, gpio;
    ssize_t sent;
    while ((opt = getopt(argc, argv, "d:n:g:a:c:f:p:o:h")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'n': r.chips = number(optarg, SM18635_MAX_CHIPS); break;
        case 'g':
            five(optarg, values, 65535);
            for (i = 0; i < 5; ++i) r.gray[i] = values[i];
            break;
        case 'a':
            five(optarg, values, 63);
            for (i = 0; i < 5; ++i) r.gain[i] = values[i];
            break;
        case 'c': r.control = number(optarg, 255); break;
        case 'f': r.din_flip = number(optarg, 1); break;
        case 'p': r.reg_control = number(optarg, 255); break;
        case 'o': dump = optarg; break;
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc || !sm18635_valid(&r) || sizeof(r) != 28) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (dump) return dump_reference(dump, &r) ? EXIT_FAILURE : EXIT_SUCCESS;
    fd = open(device, O_WRONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) { perror(device); return EXIT_FAILURE; }
    ret = ioctl(fd, SM18635_GET_ABI, 0);
    if (ret != SM18635_ABI_VERSION) {
        fprintf(stderr, "%s is not a compatible SM18635 GPIO driver (ABI=%d); no data sent\n",
                device, ret);
        close(fd);
        return EXIT_FAILURE;
    }
    gpio = ioctl(fd, SM18635_GET_GPIO, 0);
    if (gpio < 0 || gpio >= 96) {
        fprintf(stderr, "Cannot query valid output GPIO from %s; no data sent\n", device);
        close(fd);
        return EXIT_FAILURE;
    }
    printf("GPIO%d/P%c%02d: idle LOW, positive pulses, %u chip(s), two frames\n",
           gpio, 'A' + gpio / 32, gpio % 32, r.chips);
    fflush(stdout);
    /* One request == two complete frames. NEVER retry a short/interrupted
     * write: the driver might already have transmitted part of the request. */
    sent = write(fd, &r, sizeof(r));
    ret = sent == (ssize_t)sizeof(r) ? EXIT_SUCCESS : EXIT_FAILURE;
    if (sent < 0) perror("GPIO TX");
    else if (ret) fprintf(stderr, "Short GPIO request: %zd/%zu\n", sent, sizeof(r));
    if (close(fd) < 0) { perror("close"); ret = EXIT_FAILURE; }
    if (!ret) puts("GPIO TX completed; pin is low. Scope/SM18635 response not verified.");
    return ret;
}
