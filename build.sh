#!/usr/bin/env bash
# Always build the external kernel module AND userspace executable from scratch.
# Platform sources are read-only; never reuse build_module.sh's old output tree.
set -euo pipefail

project_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

usage() {
    printf '%s\n' \
        'Usage: ./build.sh' \
        'Fresh build of sm18635_gpio_drv.ko and sm18635_gpio; NOT zImage/boot.img.' \
        'Successful outputs: build/latest/{sm18635_gpio_drv.ko,sm18635_gpio}' \
        'Every invocation uses a new build/runs/clean.XXXXXXXX directory.' \
        'Optional environment:' \
        '  KERNEL_SRC            matching PRJ009 kernel source directory' \
        '  KERNEL_CONFIG         matching kernel config (uncompressed text)' \
        '  KERNEL_SYMVERS        matching kernel Module.symvers' \
        '  KERNEL_CROSS_COMPILE  kernel prefix (default mips-linux-gnu-)' \
        '  APP_CROSS_COMPILE     app prefix (default mips-linux-uclibc-)' \
        '  JOBS                  parallel jobs (default 4)'
}

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

if (( $# )); then
    if (( $# == 1 )) && [[ $1 == -h || $1 == --help ]]; then
        usage
        exit 0
    fi
    usage >&2
    exit 1
fi

for tool in make cmake find grep realpath mktemp cp mkdir sha256sum sort xargs tee ln mv date; do
    command -v "$tool" >/dev/null || die "Required tool not found: $tool"
done
jobs=${JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
kernel_prefix=${KERNEL_CROSS_COMPILE:-mips-linux-gnu-}
app_prefix=${APP_CROSS_COMPILE:-mips-linux-uclibc-}
kernel_cc=$(command -v "${kernel_prefix}gcc") || die "Compiler not found: ${kernel_prefix}gcc"
app_cc=$(command -v "${app_prefix}gcc") || die "Compiler not found: ${app_prefix}gcc"
kernel_src=$(realpath "${KERNEL_SRC:-/srv/build/users/a/al_charged/bu2_bulbcam/platform_t33a/kernel-4.4.94}")
[[ -f $kernel_src/Makefile ]] || die "No kernel Makefile in $kernel_src"
[[ $("$kernel_cc" -dumpmachine) == mips* ]] || die 'Kernel compiler must target MIPS'
[[ $("$app_cc" -dumpmachine) == mips* ]] || die 'Application compiler must target MIPS'
for input in KERNEL_CONFIG KERNEL_SYMVERS; do
    if [[ -n ${!input:-} ]]; then
        [[ -f ${!input} && -r ${!input} ]] || die "$input is not a readable file: ${!input}"
        # The helper will run from the snapshot directory: make relative inputs absolute.
        printf -v "$input" '%s' "$(realpath "${!input}")"
        export "$input"
    fi
done

# Reject redirected build roots; all writes must remain inside this project.
build_root=$(realpath -m "$project_dir/build")
[[ $build_root == "$project_dir/build" ]] || die 'build/ must not redirect outside its normal path'
runs_dir=$(realpath -m "$build_root/runs")
[[ $runs_dir == "$build_root/runs" ]] || die 'build/runs must not be a symlink to another directory'
latest="$build_root/latest"
if [[ -e $latest && ! -L $latest ]]; then
    die 'build/latest already exists and is not a symlink; refusing to overwrite it'
fi
mkdir -p "$runs_dir"
run_dir=$(mktemp -d "$runs_dir/clean.XXXXXXXX")
snapshot="$run_dir/source"
kernel_out="$snapshot/build/kernel"
app_out="$run_dir/application"
artifacts="$run_dir/artifacts"
published=0

finish() {
    local status=$?
    if (( status != 0 )); then
        printf '\nBuild failed (exit %s). Logs and work directory: %s\n' "$status" "$run_dir" >&2
        if (( ! published )); then
            printf 'build/latest was NOT updated. Any existing latest files are from an earlier build.\n' >&2
        fi
    fi
}
trap finish EXIT

# Ignore caller-supplied incremental-build directories and compiler caches.
export KERNEL_SRC="$kernel_src" KERNEL_CROSS_COMPILE="$kernel_prefix"
export CCACHE_DISABLE=1 SCCACHE_RECACHE=1
unset MAKEFLAGS MFLAGS
mkdir -p "$snapshot/kernel" "$app_out" "$artifacts"

printf '[1/4] Snapshot current working files: %s\n' "$snapshot"
cp -a -- "$project_dir/src" "$project_dir/include" "$snapshot/"
cp -- "$project_dir/CMakeLists.txt" "$project_dir/build_module.sh" "$project_dir/build.sh" "$snapshot/"
# Copy module sources only: in particular never copy .o/.ko/.cmd/Module.symvers
# or the generated *.mod.c left by an earlier manual build.
find "$project_dir/kernel" -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.s' \
       -o -name '*.lds' -o -name '*.mk' -o -name Makefile -o -name Kbuild \) \
    ! -name '*.mod.c' -print0 > "$run_dir/module-sources.list0"
while IFS= read -r -d '' source_file; do
    relative=${source_file#"$project_dir/"}
    mkdir -p "${snapshot}/${relative%/*}"
    cp -- "$source_file" "$snapshot/$relative"
done < "$run_dir/module-sources.list0"

(
    cd "$snapshot"
    sha256sum CMakeLists.txt build_module.sh build.sh
    find src include kernel -type f -print0 |
        sort -z | xargs -0 sha256sum
) > "$run_dir/sources.sha256"

printf '[2/4] Prepare fresh kernel build and compile module\n'
# Running the helper COPY makes its project_dir point at the fresh snapshot.
# Its O= and M= outputs therefore cannot reuse the original kernel/*.o or config.
(
    cd "$snapshot"
    export KERNEL_OUT="$kernel_out" MAKEFLAGS="-j$jobs"
    bash ./build_module.sh all
) 2>&1 | tee "$run_dir/kernel.log"

printf '[3/4] Configure fresh CMake cache and compile application\n'
(
    cd "$app_out"
    cmake "$snapshot" \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=mips \
        -DCMAKE_C_COMPILER="$app_cc" -DCMAKE_C_COMPILER_LAUNCHER:STRING= \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    cmake --build . -- -j "$jobs"
) 2>&1 | tee "$run_dir/application.log"

[[ -s $snapshot/kernel/sm18635_gpio_drv.ko ]] || die 'Module artifact missing'
[[ -s $app_out/sm18635_gpio && -x $app_out/sm18635_gpio ]] || die 'Application artifact missing'
# Detect edits to the selected inputs during compilation; do not publish a
# module/app pair built from source contents different from the current files.
(
    cd "$snapshot"
    sha256sum -c "$run_dir/sources.sha256"
    cd "$project_dir"
    sha256sum -c "$run_dir/sources.sha256"
) > "$run_dir/source-check.log" 2>&1

printf '[4/4] Publish successful module/application pair\n'
cp -- "$snapshot/kernel/sm18635_gpio_drv.ko" "$artifacts/sm18635_gpio_drv.ko"
cp -- "$app_out/sm18635_gpio" "$artifacts/sm18635_gpio"
cp -- "$kernel_out/.config" "$artifacts/kernel.config"
cp -- "$run_dir/sources.sha256" "$artifacts/sources.sha256"
(
    cd "$artifacts"
    sha256sum sm18635_gpio_drv.ko sm18635_gpio kernel.config > SHA256SUMS
)
{
    printf 'Built UTC: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'Project: %s\nKernel source: %s\nRun: %s\n' "$project_dir" "$kernel_src" "$run_dir"
    printf 'Kernel compiler: %s\nApplication compiler: %s\n' "$kernel_cc" "$app_cc"
    printf 'Kernel config input: %s\n' "${KERNEL_CONFIG:-PRJ009_bulbcam_defconfig (fresh)}"
    printf 'Kernel symvers input: %s\n' "${KERNEL_SYMVERS:-not provided; target compatibility unverified}"
    printf 'GPIO definition: '
    grep -E '^[[:space:]]*#[[:space:]]*define[[:space:]]+SM18635_GPIO[[:space:]]+' \
        "$snapshot/include/sm18635_protocol.h"
} > "$artifacts/BUILD_INFO.txt"

# Atomically replace only the convenience symlink. Prior complete builds stay
# in build/runs, and a failed build never exposes a half-built artifact pair.
ln -s "$artifacts" "$run_dir/latest-link"
mv -Tf -- "$run_dir/latest-link" "$latest"
published=1
printf '\nBuild successful. Fresh outputs:\n  %s/sm18635_gpio_drv.ko\n  %s/sm18635_gpio\n' "$latest" "$latest"
printf 'Logs: %s\n' "$run_dir"
printf 'Use build/latest outputs, not old kernel/*.ko or build/user-mips files.\n'
printf 'Module load compatibility and GPIO timing still require target-board verification.\n'
