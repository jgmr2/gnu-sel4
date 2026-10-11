/*
 * The live system's file system: an ext2 image linked into fs_ext2 (make PLATFORM=...),
 * writable, in RAM. RAMDISK_IMAGE is its path (system.mk).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
    .section .data.ramdisk, "aw"
    .balign 4096
    .global ramdisk_start, ramdisk_end
ramdisk_start:
    .incbin RAMDISK_IMAGE
    .balign 4096
ramdisk_end:
