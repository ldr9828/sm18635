/* SPDX-License-Identifier: GPL-2.0
 * Validation-only selectable-GPIO positive-pulse transmitter. NOT a UART driver.
 * No DT changes or direct register writes. Automatic release is restricted
 * to a verified idle, non-console Ingenic UART pinmux owner.
 */
#include <linux/delay.h>
#include <linux/console.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "sm18635_protocol.h"
/* There is no exported live-mux-owner query API in this vendor 4.4 tree.
 * Read the matching private types; do not patch the core or force ownership. */
#include "pinctrl-ingenic.h"
#include "pin_guard.h"
#include "unbind_policy.h"

#ifndef CONFIG_SOC_PRJ009
#error "This validation driver is only for the PRJ009 / T32P platform"
#endif

static DEFINE_MUTEX(tx_lock);
static struct pinctrl_dev *pin_controller;
static struct ingenic_gpio_chip *pin_chip;
static bool auto_unbind = true;
module_param(auto_unbind, bool, 0444);
MODULE_PARM_DESC(auto_unbind, "Try once to unbind an idle non-console Ingenic UART owner (default true)");
static char *auto_unbound_name;

/* No pinctrl mutex may be held across device_release_driver(): driver removal
 * releases pinctrl handles and would recursively acquire that mutex. */
static int uart_console_in_use(unsigned line)
{
    struct console *con;
    int in_use = 0;
    console_lock();
    for_each_console(con) {
        if (!(con->flags & CON_ENABLED))
            continue;
        /* Any active boot console is ambiguous; fail closed. */
        if ((con->flags & CON_BOOT) ||
            (!strcmp(con->name, "ttyS") &&
             (con->index < 0 || (unsigned)con->index == line))) {
            in_use = 1;
            break;
        }
    }
    console_unlock();
    return in_use;
}

/* Called with dev->mutex held, against this exact vendor UART implementation.
 * uart_ingenic_port starts with struct uart_port in ingenic_uart.c. */
static int check_uart_for_unbind(struct device *dev)
{
    struct uart_port *port;
    struct tty_port *tty;
    unsigned long flags;
    int line, verified, users, blocked, console, ret;

    if (!dev->driver) {
        pr_err("sm18635_gpio: %s is already unbound but its pin remains claimed; refusing blind release\n",
               dev_name(dev));
        return -EBUSY;
    }
    if (strcmp(dev->driver->name, "ingenic-uart")) {
        pr_err("sm18635_gpio: GPIO%u auto-unbind refused: %s driver=%s is not allowlisted; release it manually after checking dependencies\n",
               SM18635_GPIO, dev_name(dev), dev->driver->name);
        return -EPERM;
    }
    line = of_alias_get_id(dev->of_node, "uart");
    port = dev_get_drvdata(dev);
    verified = of_device_is_compatible(dev->of_node, "ingenic,8250-uart") &&
               line >= 0 && line <= 2 && port && port->dev == dev &&
               port->line == (unsigned)line && port->state &&
               port->mapbase == 0x10030000u + (unsigned)line * 0x1000u;
    if (!verified) {
        pr_err("sm18635_gpio: cannot verify UART state for %s; refusing auto-unbind\n", dev_name(dev));
        return -ENODEV;
    }
    tty = &port->state->port;
    mutex_lock(&tty->mutex);
    spin_lock_irqsave(&tty->lock, flags);
    users = tty->count;
    blocked = tty->blocked_open;
    console = tty->console || uart_console(port);
    spin_unlock_irqrestore(&tty->lock, flags);
    mutex_unlock(&tty->mutex);
    console |= uart_console_in_use(port->line);
    ret = sm18635_unbind_allowed(dev->driver->name, verified, console, users, blocked);
    if (ret)
        pr_err("sm18635_gpio: cannot auto-unbind ttyS%d: console=%d users=%d blocked_open=%d; leave it intact\n",
               line, console, users, blocked);
    return ret;
}

static void note_unbound_device(void)
{
    if (!auto_unbound_name)
        return;
    pr_warn("sm18635_gpio: %s remains unbound from ingenic-uart; restore manually via its driver bind node when safe\n",
            auto_unbound_name);
    kfree(auto_unbound_name);
    auto_unbound_name = NULL;
}

static int prepare_free_pin(void)
{
    struct pin_desc *pin;
    struct device *owner_dev = NULL;
    char *owner = NULL;
    int ret = 0, still_bound;

    mutex_lock(&pin_controller->mutex);
    pin = pin_desc_get(pin_controller, SM18635_GPIO);
    if (!pin) {
        ret = -ENODEV;
        goto unlock_pin;
    }
    if (pin->gpio_owner) {
        /* gpio_owner is a pinctrl range label, NOT a trustworthy device name.
         * Never gpio_free()/unexport a reservation belonging to somebody else. */
        pr_err("sm18635_gpio: GPIO%u GPIO-owned by %s; auto-unbind cannot free this reservation (sysfs exports need manual unexport)\n",
               SM18635_GPIO, pin->gpio_owner);
        ret = -EBUSY;
        goto unlock_pin;
    }
    if (!pin->mux_owner && !pin->mux_usecount && !pin->mux_setting)
        goto unlock_pin;
    if (!auto_unbind || !pin->mux_owner) {
        pr_err("sm18635_gpio: GPIO%u busy, owner=%s; auto_unbind=%d, no safe automatic release\n",
               SM18635_GPIO, pin->mux_owner ? pin->mux_owner : "unknown", auto_unbind);
        ret = -EBUSY;
        goto unlock_pin;
    }
    owner = kstrdup(pin->mux_owner, GFP_KERNEL);
    if (!owner)
        ret = -ENOMEM;
unlock_pin:
    mutex_unlock(&pin_controller->mutex);
    if (ret) {
        pr_err("sm18635_gpio: GPIO%u automatic release failed/refused (%d); no GPIO output enabled\n",
               SM18635_GPIO, ret);
        return ret;
    }
    if (!owner)
        return 0;

    owner_dev = bus_find_device_by_name(&platform_bus_type, NULL, owner);
    if (!owner_dev || owner_dev == pin_controller->dev) {
        pr_err("sm18635_gpio: cannot safely resolve platform owner %s for GPIO%u\n",
               owner, SM18635_GPIO);
        ret = -ENODEV;
        goto out;
    }
    /* Same parent/child locking order as the platform driver's sysfs unbind.
     * Users must not open/rebind the port concurrently with this operation. */
    if (owner_dev->parent)
        device_lock(owner_dev->parent);
    device_lock(owner_dev);
    ret = check_uart_for_unbind(owner_dev);
    device_unlock(owner_dev);
    if (!ret) {
        /* Recheck that the copied owner is still the selected pin's owner. */
        mutex_lock(&pin_controller->mutex);
        pin = pin_desc_get(pin_controller, SM18635_GPIO);
        if (!pin || pin->gpio_owner || !pin->mux_owner || strcmp(pin->mux_owner, owner))
            ret = -EAGAIN;
        mutex_unlock(&pin_controller->mutex);
    }
    if (!ret) {
        pr_warn("sm18635_gpio: GPIO%u attempting auto-unbind of %s; the entire UART becomes unavailable\n",
                SM18635_GPIO, owner);
        /* This API returns void. Verify device state AND live pin ownership,
         * rather than treating the call itself as proof of success. */
        device_release_driver(owner_dev);
        device_lock(owner_dev);
        still_bound = owner_dev->driver != NULL;
        device_unlock(owner_dev);
        if (still_bound) {
            ret = -EBUSY;
        } else {
            auto_unbound_name = owner;
            owner = NULL;
            mutex_lock(&pin_controller->mutex);
            ret = sm18635_pin_check_locked(pin_controller, SM18635_GPIO, 0);
            mutex_unlock(&pin_controller->mutex);
            if (!ret)
                pr_info("sm18635_gpio: auto-unbind succeeded, GPIO%u is now free\n", SM18635_GPIO);
        }
    }
    if (owner_dev->parent)
        device_unlock(owner_dev->parent);
out:
    if (ret)
        pr_err("sm18635_gpio: GPIO%u automatic release failed/refused (%d); no GPIO output enabled\n",
               SM18635_GPIO, ret);
    if (owner_dev)
        put_device(owner_dev);
    kfree(owner);
    return ret;
}

static int resolve_pin_controller(void)
{
    struct gpio_desc *desc;
    struct gpio_chip *chip;
    struct ingenic_pinctrl *pctl;
    if (SM18635_GPIO >= 96u || !gpio_is_valid(SM18635_GPIO))
        return -EINVAL;
    desc = gpio_to_desc(SM18635_GPIO);
    if (!desc)
        return -ENODEV;
    chip = gpiod_to_chip(desc);
    if (!chip || !chip->dev ||
        !of_device_is_compatible(chip->dev->of_node, "ingenic,PRJ009-pinctrl") ||
        chip->base < 0 || chip->base + chip->ngpio <= SM18635_GPIO ||
        chip->base > SM18635_GPIO || gpio_cansleep(SM18635_GPIO))
        return -ENODEV;
    pin_chip = gc_to_ingenic_gc(chip);
    pctl = pin_chip->pctl;
    if (!pctl || pctl->dev != chip->dev || !pctl->pctl_dev ||
        pin_chip->idx != SM18635_GPIO / 32u ||
        pin_chip->grange.base != pin_chip->grange.pin_base)
        return -ENODEV;
    pin_controller = pctl->pctl_dev;
    return 0;
}

/* Caller holds pinctrl mutex. Read-only verification catches pinmux changes
 * made outside the ownership bookkeeping. Never try to repair a stolen pin. */
static int check_output_mode(void)
{
    u32 bit = BIT(SM18635_GPIO % 32u);
    if ((ingenic_gpio_readl(pin_chip, PxINT) & bit) ||
        !(ingenic_gpio_readl(pin_chip, PxMSK) & bit) ||
        (ingenic_gpio_readl(pin_chip, PxPAT1) & bit)) {
        pr_err("sm18635_gpio: GPIO%u is no longer GPIO output; refusing TX\n", SM18635_GPIO);
        return -EBUSY;
    }
    return 0;
}

static void idle_if_still_owned(void)
{
    mutex_lock(&pin_controller->mutex);
    if (!sm18635_pin_check_locked(pin_controller, SM18635_GPIO, 1) &&
        !check_output_mode())
        gpio_set_value(SM18635_GPIO, 0);
    mutex_unlock(&pin_controller->mutex);
}

/* Diagnostic only: this is checked AFTER the bounded burst, not a hard
 * watchdog. Do not add printk, allocation or time sampling between bits. */
#define BURST_WARNING_NS 2000000ull

static int send_burst(const struct sm18635_pulse *p, unsigned count, u64 *elapsed)
{
    unsigned long flags;
    unsigned i;
    u64 start;

    preempt_disable();
    local_irq_save(flags);
    start = ktime_get_ns();
    for (i = 0; i < count; ++i) {
        gpio_set_value(SM18635_GPIO, 1);
        ndelay(p[i].high_ns);
        gpio_set_value(SM18635_GPIO, 0);
        ndelay(p[i].low_ns);
    }
    /* Last edge is always low, before interrupts/preemption are restored. */
    *elapsed = ktime_get_ns() - start;
    local_irq_restore(flags);
    preempt_enable();

    return *elapsed > BURST_WARNING_NS ? -ETIMEDOUT : 0;
}

static ssize_t sm18635_write(struct file *file, const char __user *buffer,
                             size_t length, loff_t *position)
{
    struct sm18635_request request;
    struct sm18635_pulse *pulses;
    unsigned count, frame;
    u64 elapsed[2] = {0, 0};
    int ret;

    (void)file;
    (void)position;
    if (length != sizeof(request))
        return -EMSGSIZE;
    if (copy_from_user(&request, buffer, sizeof(request)))
        return -EFAULT;
    if (!sm18635_valid(&request))
        return -EINVAL;
    pulses = kmalloc_array(SM18635_MAX_PULSES, sizeof(*pulses), GFP_KERNEL);
    if (!pulses)
        return -ENOMEM;
    count = sm18635_build_pulses(pulses, &request);
    ret = mutex_lock_interruptible(&tx_lock);
    if (ret)
        goto free_pulses;
    ret = mutex_lock_interruptible(&pin_controller->mutex);
    if (ret)
        goto unlock_tx;
    ret = sm18635_pin_check_locked(pin_controller, SM18635_GPIO, 1);
    if (!ret)
        ret = check_output_mode();
    if (ret)
        goto unlock_pin;
    /* Block normal pinctrl reconfiguration for the entire request, including
     * reset gaps. GPIO value writes do not take this mutex in this platform. */

    for (frame = 0; frame < 2; ++frame) {
        gpio_set_value(SM18635_GPIO, 0);
        /* Sleeping is safe in reset low; scheduling can only lengthen it. */
        usleep_range(SM18635_RESET_US, SM18635_RESET_US + 200);
        if (signal_pending(current)) {
            ret = -EINTR;
            break;
        }
        ret = send_burst(pulses, count, &elapsed[frame]);
        usleep_range(SM18635_RESET_US, SM18635_RESET_US + 200);
        if (ret || signal_pending(current)) {
            if (!ret)
                ret = -EINTR;
            break;
        }
    }
    gpio_set_value(SM18635_GPIO, 0);
unlock_pin:
    mutex_unlock(&pin_controller->mutex);
unlock_tx:
    mutex_unlock(&tx_lock);
    if (ret)
        pr_warn_ratelimited("sm18635_gpio: GPIO%u TX failed (%d), burst ns=%llu/%llu\n",
                            SM18635_GPIO, ret, (unsigned long long)elapsed[0],
                            (unsigned long long)elapsed[1]);
    else
        pr_info_ratelimited("sm18635_gpio: GPIO%u two frames, burst ns=%llu/%llu; verify pin timing with scope\n",
                            SM18635_GPIO, (unsigned long long)elapsed[0],
                            (unsigned long long)elapsed[1]);
free_pulses:
    kfree(pulses);
    return ret ? ret : (ssize_t)sizeof(request);
}

static long sm18635_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    (void)file;
    (void)arg;
    /* Metadata handshake only; this does not receive anything from DIN. */
    switch (cmd) {
    case SM18635_GET_ABI: return SM18635_ABI_VERSION;
    case SM18635_GET_GPIO: return SM18635_GPIO;
    default: return -ENOTTY;
    }
}

static const struct file_operations sm18635_fops = {
    .owner = THIS_MODULE,
    .open = nonseekable_open,
    .write = sm18635_write,
    .unlocked_ioctl = sm18635_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = sm18635_ioctl,
#endif
    .llseek = no_llseek,
};

static struct miscdevice sm18635_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "sm18635_gpio",
    .fops = &sm18635_fops,
    .mode = 0600,
};

static int __init sm18635_init(void)
{
    int ret;

    BUILD_BUG_ON(sizeof(struct sm18635_request) != 28);
    if (!of_machine_is_compatible("ingenic,PRJ009"))
        return -ENODEV;
    ret = resolve_pin_controller();
    if (ret) {
        pr_err("sm18635_gpio: GPIO%u invalid or live pinctrl cannot be verified (%d)\n",
               SM18635_GPIO, ret);
        return ret;
    }
    ret = prepare_free_pin();
    if (ret) {
        note_unbound_device();
        return ret;
    }
    /* Reserve first WITHOUT changing direction/level. A second GPIO claimant
     * is rejected by gpiolib, including an existing sysfs export. */
    ret = gpio_request(SM18635_GPIO, "sm18635_gpio");
    if (ret) {
        pr_err("sm18635_gpio: GPIO%u request failed (%d); check GPIO/sysfs owner\n",
               SM18635_GPIO, ret);
        note_unbound_device();
        return ret;
    }
    mutex_lock(&pin_controller->mutex);
    ret = sm18635_pin_check_locked(pin_controller, SM18635_GPIO, 1);
    mutex_unlock(&pin_controller->mutex);
    if (ret)
        goto free_gpio;
    /* This API itself takes pinctrl mutex, so cannot be called while holding
     * it. Do not bind/unbind pin consumers concurrently with module loading. */
    ret = gpio_direction_output(SM18635_GPIO, 0);
    if (ret)
        goto free_gpio;
    mutex_lock(&pin_controller->mutex);
    ret = sm18635_pin_check_locked(pin_controller, SM18635_GPIO, 1);
    if (!ret)
        ret = check_output_mode();
    mutex_unlock(&pin_controller->mutex);
    if (ret)
        goto free_gpio;
    ret = misc_register(&sm18635_device);
    if (ret) {
        idle_if_still_owned();
        goto free_gpio;
    }
    pr_warn("sm18635_gpio: validation only, GPIO%u/P%c%02u idle LOW, positive pulses, max 4 chips; IRQ-off bursts affect system latency\n",
            SM18635_GPIO, 'A' + SM18635_GPIO / 32u, SM18635_GPIO % 32u);
    return 0;
free_gpio:
    gpio_free(SM18635_GPIO);
    note_unbound_device();
    return ret;
}

static void __exit sm18635_exit(void)
{
    misc_deregister(&sm18635_device);
    idle_if_still_owned();
    gpio_free(SM18635_GPIO);
    /* Do not restore any previous peripheral mux automatically. */
    note_unbound_device();
}

module_init(sm18635_init);
module_exit(sm18635_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PRJ009 selectable GPIO SM18635 positive-pulse validation driver");
MODULE_AUTHOR("SM18635 validation demo");
