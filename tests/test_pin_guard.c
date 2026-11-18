/* Exercise the actual kernel ownership decision with a fake descriptor.
 * This is not a test of kernel private-structure ABI or pinmux races. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

struct pin_desc {
    unsigned mux_usecount;
    const char *mux_owner;
    const void *mux_setting;
    const char *gpio_owner;
};
struct pinctrl_dev { struct pin_desc *pin; };
static struct pin_desc *pin_desc_get(struct pinctrl_dev *p, unsigned gpio)
{
    (void)gpio;
    return p->pin;
}
#define pr_err(...) fprintf(stderr, __VA_ARGS__)
#include "../kernel/pin_guard.h"

static void expect(int actual, int wanted)
{
    if (actual != wanted) {
        fprintf(stderr, "guard returned %d, expected %d\n", actual, wanted);
        exit(1);
    }
}

int main(void)
{
    struct pin_desc pin = {0};
    struct pinctrl_dev controller = {&pin};
    const char *owners[] = {"10030000.serial", "13490000.sfc1-spi", "10051000.i2c", "pwm"};
    unsigned i;
    expect(sm18635_pin_check_locked(&controller, 72, 0), 0);
    expect(sm18635_pin_check_locked(&controller, 72, 1), -EIO);
    for (i = 0; i < sizeof(owners)/sizeof(owners[0]); ++i) {
        pin.mux_owner = owners[i];
        expect(sm18635_pin_check_locked(&controller, 72, 0), -EBUSY);
        pin.gpio_owner = "GPC:72";
        expect(sm18635_pin_check_locked(&controller, 72, 1), -EBUSY);
        pin.gpio_owner = NULL;
    }
    pin.mux_owner = NULL;
    pin.mux_usecount = 1;
    expect(sm18635_pin_check_locked(&controller, 72, 0), -EBUSY);
    pin.mux_usecount = 0;
    pin.mux_setting = &pin;
    expect(sm18635_pin_check_locked(&controller, 72, 0), -EBUSY);
    pin.mux_setting = NULL;
    pin.gpio_owner = "sysfs or another GPIO consumer";
    expect(sm18635_pin_check_locked(&controller, 72, 0), -EBUSY);
    expect(sm18635_pin_check_locked(&controller, 72, 1), 0);
    pin.gpio_owner = NULL;
    expect(sm18635_pin_check_locked(&controller, 72, 0), 0);
    controller.pin = NULL;
    expect(sm18635_pin_check_locked(&controller, 72, 0), -ENODEV);
    puts("PASS: free pin, any mux owner, GPIO owner, lost reservation and missing descriptor");
    return 0;
}
