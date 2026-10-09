// Copyright 2026, UNSW
// SPDX-License-Identifier: BSD-2-Clause


DefinitionBlock ("", "DSDT", 2, "libvmm", "libvmm", 0x1)
{
    Scope (\_SB)
    {
        Device (PCI0)
        {
            // PCI root bridge
            Name (_HID, EisaId ("PNP0A03"))
            // Compatible with PCIe root
            Name (_CID, EisaId ("PNP0A08"))
            Name (_UID, One)
            // PCI segment and base bus number 0
            Name (_SEG, Zero)
            Name (_BBN, Zero)

            Name (_CRS, ResourceTemplate ()
            {
                // Bus numbers this root bridge owns
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000,         // Granularity
                    0x0000,         // Min
                    0x0000,         // Max
                    0x0000,         // Translation
                    0x0001          // Length
                )

                // Prefetchable MMIO window
                QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, Cacheable, ReadWrite,
                    0x0000000000000000, // Granularity
                    0xC0000000, // Min (sobre los 2 GiB de RAM del invitado)
                    0xC007ffff, // Max
                    0x0000000000000000, // Translation
                    0x80000  // Length
                )
            })
        }
    }

    Scope(\_SB) {
        Scope(PCI0) {
            // PCI Routing Table
            Method(_PRT, 0) {
                Return (Package() {
                    // Virtio console:
                    // Device 0x3, function 0, INTA -> GSI 9
                    Package() { 0x00030000, 0, 0, 9 },
                    // Virtio net:
                    // Device 0x4, function 0, INTA -> GSI 10
                    Package() { 0x00040000, 0, 0, 10 },
                    // Virtio blk:
                    // Device 0x5, function 0, INTA -> GSI 11
                    Package() { 0x00050000, 0, 0, 11 },
                    // Puente con GNU/seL4 (segunda consola virtio, /dev/hvc1):
                    // Device 0x6, function 0, INTA -> GSI 12
                    Package() { 0x00060000, 0, 0, 12 },
                })
            }
        }
    }
}