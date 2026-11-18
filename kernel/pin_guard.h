#ifndef SM18635_PIN_GUARD_H
#define SM18635_PIN_GUARD_H

/* Caller holds pctldev->mutex. Use live pinctrl ownership, never the vendor
 * used_pins_bitmap (which can remain set after an old mux owner is gone).
 * Types/helpers come from the matching kernel pinctrl headers, or test stubs. */
static int sm18635_pin_check_locked(struct pinctrl_dev *pctldev,
                                    unsigned gpio, int requested_by_us)
{
    struct pin_desc *pin = pin_desc_get(pctldev, gpio);
    if (!pin) {
        pr_err("sm18635_gpio: GPIO%u has no pinctrl descriptor\n", gpio);
        return -ENODEV;
    }
    if (pin->mux_usecount || pin->mux_owner || pin->mux_setting) {
        pr_err("sm18635_gpio: GPIO%u mux occupied by %s (refs=%u); release that owner first\n",
               gpio, pin->mux_owner ? pin->mux_owner : "unknown", pin->mux_usecount);
        return -EBUSY;
    }
    if (!requested_by_us && pin->gpio_owner) {
        pr_err("sm18635_gpio: GPIO%u already GPIO-owned by %s\n", gpio, pin->gpio_owner);
        return -EBUSY;
    }
    if (requested_by_us && !pin->gpio_owner) {
        pr_err("sm18635_gpio: GPIO%u lost its GPIO reservation\n", gpio);
        return -EIO;
    }
    return 0;
}

#endif
