#!/bin/bash
# Rebuild the PRJ009 image used by the current bu2_bulbcam release.
set -euo pipefail
repo=${BULBCAM_ROOT:-/home/a/hl/bu2_bulbcam}
kernel="$repo/platform_t33a/kernel-4.4.94"
release="$repo/release"
output="$repo/output"
compiler=${KERNEL_CROSS_COMPILE:-/home/a/toolchain/mips-gcc540-glibc222-r3.3.7.mxu2.cve2/bin/mips-linux-gnu-}
stage=$(mktemp -d /tmp/bulbcam-spi1-build.XXXXXX)
echo "Build staging directory: $stage"
mkdir -p "$stage/boot" "$stage/out" "$stage/keys" "$output"
make -C "$kernel" ARCH=mips CROSS_COMPILE="$compiler" PRJ009_bulbcam_defconfig
make -C "$kernel" -j"${JOBS:-8}" ARCH=mips CROSS_COMPILE="$compiler" HOSTCFLAGS=-fcommon zImage
for option in SPI INGENIC_SFC1_SSI INGENIC_SFC1_SSI_DMA SPI_SPIDEV; do
    grep -qx "CONFIG_${option}=y" "$kernel/.config"
done
cp "$kernel/arch/mips/boot/compressed/zImage" "$stage/boot/kernel"
cp "$kernel/arch/mips/boot/dts/ingenic/bulbcam_prj009.dtb" "$stage/boot/dtb"
cp "$release/release/image/boot/boot.its" "$stage/boot/boot.its"
cp "$release/release/image/out/ramdisk" "$stage/out/ramdisk"
ln -s "$release/build/keys/dev.key" "$stage/keys/dev.key"
openssl req -new -x509 -sha256 -key "$stage/keys/dev.key" \
    -out "$stage/keys/dev.crt" -days 36500 -subj /OU=Camera
mkimage -f "$stage/boot/boot.its" -k "$stage/keys" -r "$stage/boot.img"
# Verify against the public key in the existing release's U-Boot DTB.
"$release/build/tools/fit_check_sign" -f "$stage/boot.img" \
    -k "$release/release/image/out/u-boot-tmp.dtb"
test "$(stat -c %s "$stage/boot.img")" -lt $((5184 * 1024))
if [ -f "$output/boot.img" ]; then
    cp -p "$output/boot.img" "$stage/previous-boot.img"
fi
cp "$stage/boot.img" "$output/boot.img"
cp "$kernel/.config" "$output/boot-spi1.config"
(cd "$output" && sha256sum boot.img > boot.img.sha256)
echo "Ready: $output/boot.img"
