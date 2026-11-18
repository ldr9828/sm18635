obj-m += sm18635_gpio_drv.o
ccflags-y += -I$(src)/../include
# Read-only use of this platform's private pinctrl ownership structures.
# The module MUST be built against the actual running kernel's source/config.
ccflags-y += -I$(srctree)/drivers/pinctrl
