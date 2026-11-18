#ifndef SM18635_GPIO_PROTOCOL_H
#define SM18635_GPIO_PROTOCOL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SM18635_ABI_VERSION 2u
#define SM18635_GET_ABI _IO('S', 0x35)
#define SM18635_GET_GPIO _IO('S', 0x36)
/* Change ONLY this definition to choose the output. PA=n, PB=32+n, PC=64+n.
 * The userspace executable queries the running module, not this constant. */
#define SM18635_GPIO 47u
/* Keep the IRQ-off burst short for this validation-only bit-bang driver. */
#define SM18635_MAX_CHIPS 4u
#define SM18635_MAX_BITS (96u * SM18635_MAX_CHIPS + 48u)
#define SM18635_MAX_PULSES (1u + SM18635_MAX_BITS)
#define SM18635_RESET_US 512u

/* Fixed 28-byte native-endian ABI; no pointers, bitfields or implicit padding. */
struct sm18635_request {
    __u32 version;
    __u32 chips;
    __u16 gray[5];
    __u8 gain[5];
    __u8 control;
    __u8 din_flip;
    __u8 reg_control;
    __u8 reserved[2];
};

struct sm18635_pulse {
    __u16 high_ns;
    __u16 low_ns;
};

static inline int sm18635_valid(const struct sm18635_request *r)
{
    unsigned i;
    if (r->version != SM18635_ABI_VERSION || !r->chips ||
        r->chips > SM18635_MAX_CHIPS || r->din_flip > 1 ||
        r->reserved[0] || r->reserved[1])
        return 0;
    for (i = 0; i < 5; ++i)
        if (r->gain[i] > 63)
            return 0;
    return 1;
}

static inline void sm18635_append(struct sm18635_pulse *p, unsigned *n,
                                  unsigned value, unsigned bits)
{
    while (bits--) {
        int one = (value & (1u << bits)) != 0;
        p[*n].high_ns = one ? 1500 : 500;
        p[*n].low_ns = one ? 500 : 1500;
        ++*n;
    }
}

/* Caller supplies SM18635_MAX_PULSES entries. Reset is handled outside the
 * critical burst, as extra low idle time is allowed. All fields are MSB first. */
static inline unsigned sm18635_build_pulses(struct sm18635_pulse *p,
                                            const struct sm18635_request *r)
{
    unsigned n = 1, chip, ch;
    if (!sm18635_valid(r))
        return 0;
    p[0].high_ns = 16000;
    p[0].low_ns = 4000;
    for (chip = 0; chip < r->chips; ++chip) {
        sm18635_append(p, &n, 0x96, 8);
        for (ch = 0; ch < 5; ++ch)
            sm18635_append(p, &n, r->gray[ch], 16);
        sm18635_append(p, &n, r->control, 8);
    }
    sm18635_append(p, &n, 0xe1, 8);
    for (ch = 0; ch < 5; ++ch)
        sm18635_append(p, &n, r->gain[ch], 6);
    sm18635_append(p, &n, (unsigned)r->din_flip << 1, 2);
    sm18635_append(p, &n, r->reg_control, 8);
    return n;
}

#endif
