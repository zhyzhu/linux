# Samsung Galaxy SL GT-I9003 (Latona)

Linux 6.12.114 with the Latona port used for Maemo Leste development.
The baseline commit imports the upstream release archive.

Build with an ARM GNU cross toolchain and the Linux kernel build dependencies:

```sh
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- O=out latona_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- O=out -j8 zImage modules \
  ti/omap/omap3-samsung-latona.dtb ti/omap/omap3-samsung-latona-internal.dtb
```

Use the internal DTB for the eMMC variant. The Samsung boot container,
initramfs and Maemo Leste root filesystem are separate build inputs.
The SGX kernel configuration supports a separately built SGX module.
Board adjustments and bring-up diagnostics remain in the port.
