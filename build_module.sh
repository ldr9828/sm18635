#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kernel_src=${KERNEL_SRC:-/srv/build/users/a/al_charged/bu2_bulbcam/platform_t33a/kernel-4.4.94}
kernel_src=$(realpath "$kernel_src")
kernel_out=$(realpath -m "${KERNEL_OUT:-$project_dir/build/kernel}")
cross_prefix=${KERNEL_CROSS_COMPILE:-mips-linux-gnu-}
action=${1:-all}

# Never generate files in the platform repository or any other output tree.
case "$kernel_out" in
    "$project_dir"/build/*) ;;
    *) echo 'KERNEL_OUT must be inside this project/build/' >&2; exit 1 ;;
esac
test -f "$kernel_src/Makefile"
command -v "${cross_prefix}gcc" >/dev/null
command -v grep >/dev/null
mkdir -p "$kernel_out"

kernel_make()
{
    make -C "$kernel_src" O="$kernel_out" ARCH=mips \
        CROSS_COMPILE="$cross_prefix" "$@"
}

prepare()
{
    if [ -n "${KERNEL_CONFIG:-}" ]; then
        cp -- "$KERNEL_CONFIG" "$kernel_out/.config"
        kernel_make olddefconfig
    elif [ ! -f "$kernel_out/.config" ]; then
        kernel_make PRJ009_bulbcam_defconfig
    fi
    kernel_make modules_prepare
    if [ -n "${KERNEL_SYMVERS:-}" ]; then
        cp -- "$KERNEL_SYMVERS" "$kernel_out/Module.symvers"
    fi
}

build()
{
    test -f "$kernel_out/include/generated/autoconf.h" || {
        echo 'Run: sh build_module.sh prepare' >&2; exit 1;
    }
    if grep -q '^CONFIG_MODVERSIONS=y$' "$kernel_out/.config" && \
       [ ! -s "$kernel_out/Module.symvers" ]; then
        echo 'CONFIG_MODVERSIONS=y: supply the matching KERNEL_SYMVERS first' >&2
        exit 1
    fi
    if [ ! -s "$kernel_out/Module.symvers" ]; then
        echo 'WARNING: no matching Module.symvers; build proves compilation only, not load compatibility' >&2
    fi
    kernel_make M="$project_dir/kernel" modules
    echo "Module: $project_dir/kernel/sm18635_gpio_drv.ko"
    echo 'Verify running-kernel config, release and symbol compatibility before loading. Never force-load.'
}

case "$action" in
    prepare) prepare ;;
    build) build ;;
    all) prepare; build ;;
    *) echo 'Usage: sh build_module.sh [prepare|build|all]' >&2; exit 1 ;;
esac
