#ifndef SM18635_UNBIND_POLICY_H
#define SM18635_UNBIND_POLICY_H

/* Deliberately narrow: arbitrary provider removal can take down storage,
 * power rails or dependent devices. Add a class only after auditing removal. */
static int sm18635_unbind_allowed(const char *driver, int verified_uart,
                                  int console, int users, int blocked_open)
{
    if (!driver || strcmp(driver, "ingenic-uart"))
        return -EPERM;
    if (!verified_uart)
        return -ENODEV;
    if (console || users || blocked_open)
        return -EBUSY;
    return 0;
}

#endif
