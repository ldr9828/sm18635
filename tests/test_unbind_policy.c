#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/unbind_policy.h"

static void expect(int actual, int wanted)
{
    if (actual != wanted) {
        fprintf(stderr, "unbind policy returned %d, expected %d\n", actual, wanted);
        exit(1);
    }
}

int main(void)
{
    const char *unsupported[] = {NULL, "ingenic-sfc", "ingenic-sfc-spi",
                                "ingenic-mmc", "ingenic-i2c", "ingenic-pwm", "unknown"};
    unsigned i;
    expect(sm18635_unbind_allowed("ingenic-uart", 1, 0, 0, 0), 0);
    expect(sm18635_unbind_allowed("ingenic-uart", 0, 0, 0, 0), -ENODEV);
    expect(sm18635_unbind_allowed("ingenic-uart", 1, 1, 0, 0), -EBUSY);
    expect(sm18635_unbind_allowed("ingenic-uart", 1, 0, 1, 0), -EBUSY);
    expect(sm18635_unbind_allowed("ingenic-uart", 1, 0, 0, 1), -EBUSY);
    expect(sm18635_unbind_allowed("ingenic-uart", 1, 0, -1, 0), -EBUSY);
    for (i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i)
        expect(sm18635_unbind_allowed(unsupported[i], 1, 0, 0, 0), -EPERM);
    puts("PASS: only verified idle non-console UART may auto-unbind; critical/unknown drivers rejected");
    return 0;
}
