# SPDX-License-Identifier: BSD-2-Clause
# Generates the GNU/seL4 system description (gnu-sel4.system) and the sDDF configuration.
#
#                       ┌─ posix_server ── processes (created on demand from its untyped)
#   console (COM1) ─────┤      │ PPC
#   serial_virt_rx/tx   │   fs_ext2 ──┐
#                       └─ CLIENT_VMM ── Linux VM (Docker)             disk (virtio-blk)
#                              │    └──────────────────────────────── blk_virt: partition 1 = ext2,
#                              │                                                 partition 2 = Docker
#   net (virtio-net) ── net_virt_rx/tx ┬─ network_copy ── CLIENT_VMM
#                                      └─ netstack_copy ── netstack (FreeBSD TCP/IP) ── posix_server
#
# Bridge: posix_server <-> CLIENT_VMM (our own queues, /dev/hvc1 in the VM) to run Linux
# commands from GNU/seL4 (/bin/linux, /bin/docker), like WSL interop.
#
# Console: two clients of the serial virtualiser. Ctrl-\ followed by the number and Enter
# switches where the keyboard goes: 0 = GNU/seL4, 1 = the VM.
#
# Usage: meta.py --sddf <sddf> --board x86_64_generic_vtx --output <dir> --sdf gnu-sel4.system
import argparse
import os
import re
from sdfgen import SystemDescription, Sddf, Vmm, DeviceTree
from board import BOARDS, add_x86_hpet

ProtectionDomain = SystemDescription.ProtectionDomain
VirtualMachine = SystemDescription.VirtualMachine
MemoryRegion = SystemDescription.MemoryRegion
Map = SystemDescription.Map
Channel = SystemDescription.Channel
IrqIoapic = SystemDescription.IrqIoapic

# ---- native GNU/seL4 (must match include/sel4posix/*.h and server/server.h) ----
PROC_SLOTS = 128                      # PROC_SLOTS in procmem.h: scheduling contexts in the pool
SERVER_FS_CHANNEL_ID = 21             # FS_CH
SERVER_BRIDGE_CHANNEL_ID = 22         # BRIDGE_CH in server/server.h
SERVER_NET_CHANNEL_ID = 23            # NET_CH in server/server.h
NETSTACK_SERVER_CHANNEL_ID = 50       # SERVER_CH in net/netstack.c
SERVER_LEASE_CHANNEL_ID = 24          # NET_LEASE_CH in server/server.h (DHCP lease notification)
NETSTACK_LEASE_CHANNEL_ID = 51        # LEASE_CH in net/netstack.c
NET_BUF_SIZE = 0x1_0000               # NET_BUF_SIZE in include/sel4posix/net_protocol.h
VMM_BRIDGE_CHANNEL_ID = 50            # BRIDGE_CH in vm/guest_arch_init.h
BRIDGE_DATA_SIZE = 0x1_0000           # BR_DATA_SIZE / BRIDGE_DATA_SIZE
FS_BUF_SIZE = 0x10_0000              # FS_BUF_SIZE in fs_protocol.h: 1 MiB
FS_CACHE_SIZE = 0x200_0000            # 32 MiB: sDDF data region, fs_ext2 cache (+ 1 MiB staging). 64 MiB: 16384 4 KiB frames, the capDL initializer runs out of slots (its address is not 2 MiB aligned for large pages)

# RAM budget: seL4 hands out boot memory as "dirty" untyped and zeroes it IN FULL the first
# time it is used, in power-of-2 blocks. On q35 the low RAM is one 2 GiB block and the high RAM
# starts with another 2-4 GiB one: if everything (process memory + VM + images) does not fit in
# low RAM (~1.85 GiB free), the high block gets touched too and QEMU grows from ~2.2 to 4.3 GB
# or more. Today: processes 768 MiB + VM 768 MiB + netstack 64 MiB + fs_ext2's cache 32 MiB +
# ~50 MiB of drivers.
NETSTACK_HEAP_SIZE = 0x400_0000       # NETSTACK_HEAP_SIZE in net/netstack.h

# posix_server's root CSpace for the memory manager (= SRV_* in procmem.h). <cspace>
# elements of our own Microkit (third_party/microkit, branch gnu-sel4): sdfgen does not know them.
SRV_ROOT_CNODE_SLOT = 1                # its own root CNode: it creates new CNodes in 20..63
SRV_POOL_SLOT = 2                      # untypeds for process memory (powers of 2)
SRV_POOL_SIZES = [0x2000_0000, 0x1000_0000]                  # 512 + 256 MiB
SRV_ASID_POOL_SLOT = 5
SRV_TCB_SLOT = 6
SRV_EP_SLOT = 7
SRV_SCHED_POOL_SLOT = 8

# ---- VM (must match vm/guest_arch_init.c) ----
GUEST_RAM_SIZE = 0x3000_0000          # 768 MiB
GUEST_RAM_VMM_VADDR = 0x1_0000_0000
VM_DISK_DATA_SIZE = 0x100_0000        # 16 MiB disk buffer for the VM

# Platform (make PLATFORM=..., system.mk): "qemu", or "latitude7390": a live system in RAM for
# the real Dell Latitude 7390 2-in-1 (no disk subsystem: fs_ext2 holds the file system; a network
# driver with no card; the firmware's framebuffer; no QEMU-only I/O ports)
PLATFORM = os.environ.get("PLATFORM", "qemu")
LIVE = PLATFORM != "qemu"
# make LINUX_VM=no: no VM (no VMM, no bridge, no VM network client or console), for a kernel
# without VT-x (board x86_64_generic), e.g. on CI runners
LINUX_VM = os.environ.get("LINUX_VM", "yes") == "yes"

# Console: "graphical" (screen + PS/2 keyboard, console/fbcon.c) or "serial" (UART COM1).
# Both are the sDDF "serial driver": the rest of the system does not change. make CONSOLE=serial
CONSOLE = os.environ.get("CONSOLE", "graphical")
# bochs-display screen in slot 5 (make run): BAR0 framebuffer, BAR2 registers
FB_PADDR, FB_SIZE = 0xFD00_0000, 0x40_0000            # 1280x800x32 fits in 4 MiB
FB_REGS_PADDR = 0xFEBC_A000
if PLATFORM == "latitude7390":
    # UHD Graphics 620 (00:02.0): BAR2 is the 256 MiB aperture at 0x60000000, where UEFI's GOP
    # framebuffer starts; 1920x1080x32, stride 7680 bytes (read from the running laptop)
    FB_PADDR, FB_SIZE = 0x6000_0000, 0x80_0000
    FB_REGS_PADDR = None
KBD_IRQ_CH = 10                       # KBD_IRQ_CH in console/fbcon.c

# ---- QEMU q35 devices (fixed PCI slots: the firmware assigns these BARs and IRQs) ----
# With more than 4 GiB of RAM (make run uses 6 GiB) and 39 physical address bits (QEMU's -cpu ...,
# phys-bits=39: the window depends on them) the firmware puts the 64-bit BARs in the high window
# 0x7000000000, always in this order:
#   virtio-blk disk    in slot 3: BAR4 0x7000000000, IRQ 11
#   virtio-net network in slot 4: BAR4 0x7000004000, IRQ 10
BLK_BAR, BLK_IRQ = 0x70_0000_0000, 11
NET_BAR, NET_IRQ = 0x70_0000_4000, 10
# Queues of the sDDF virtio drivers (addresses fixed in the drivers)
VIRTIO_BLK_REQUESTS_PADDR = 0x5FDF_0000
VIRTIO_BLK_METADATA_PADDR = 0x5FFF_0000
VIRTIO_NET_VQUEUES_PADDR = 0x7A00_0000


def add_region(sdf, pd, name, size, paddr, vaddr, cached):
    mr = MemoryRegion(sdf, name, size, paddr=paddr)
    sdf.add_mr(mr)
    pd.add_map(Map(mr, vaddr, "rw", cached=cached))


def server_cspace(sdf, extra=""):
    """The system description with posix_server's own CSpace (our Microkit's <cspace>)"""
    caps = [f'<cap_cnode slot="{SRV_ROOT_CNODE_SLOT}" pd="posix_server" />']
    caps += [f'<cap_untyped slot="{SRV_POOL_SLOT + i}" size="{size:#x}" />' for i, size in enumerate(SRV_POOL_SIZES)]
    caps += [f'<cap_asid_pool slot="{SRV_ASID_POOL_SLOT}" />',
             f'<cap_tcb slot="{SRV_TCB_SLOT}" pd="posix_server" />',
             f'<cap_endpoint slot="{SRV_EP_SLOT}" pd="posix_server" />',
             f'<cap_sched_pool slot="{SRV_SCHED_POOL_SLOT}" count="{PROC_SLOTS}" />']
    xml, n = re.subn(r'(<protection_domain name="posix_server"[^>]*>)',
                     lambda m: m.group(1) + "\n        <cspace>" + "".join(caps) + "</cspace>" + extra, sdf.render())
    assert n == 1
    return xml


# ---- QEMU's ARM virt machine (make PLATFORM=qemu-arm; TODO item 16, milestone 2) ----
RTC_PL031_PADDR = 0x0901_0000         # its PL031 real-time clock (server/clock.c: rtc_base)


def generate_arm(sdf, sdf_file, output_dir, board, dtb):
    """The native system: the serial console, ARM's timer, the disk (virtio-blk over MMIO),
    fs_ext2, posix_server. No network or VM yet"""
    serial_driver = ProtectionDomain("serial_driver", "serial_driver.elf", priority=200)
    serial_virt_tx = ProtectionDomain("serial_virt_tx", "serial_virt_tx.elf", priority=199)
    serial_virt_rx = ProtectionDomain("serial_virt_rx", "serial_virt_rx.elf", priority=199, stack_size=0x2000)
    timer_driver = ProtectionDomain("timer_driver", "timer_driver.elf", priority=254)
    blk_driver = ProtectionDomain("blk_driver", "blk_driver.elf", priority=200, stack_size=0x2000)
    blk_virt = ProtectionDomain("blk_virt", "blk_virt.elf", priority=199, stack_size=0x2000)
    fs = ProtectionDomain("fs_ext2", "fs_ext2.elf", priority=160, stack_size=0x8000)
    server = ProtectionDomain("posix_server", "posix_server.elf", priority=150, stack_size=0x10000)

    fs_buf = MemoryRegion(sdf, "fs_buf", FS_BUF_SIZE)
    sdf.add_mr(fs_buf)
    server.add_map(Map(fs_buf, 0x3100_0000, "rw", setvar_vaddr="fs_buf"))
    fs.add_map(Map(fs_buf, 0x3100_0000, "rw", setvar_vaddr="fs_buf"))
    # What posix_server maps whether or not anyone is at the other end (netstack, the VM)
    net_buf = MemoryRegion(sdf, "net_buf", NET_BUF_SIZE)
    sdf.add_mr(net_buf)
    server.add_map(Map(net_buf, 0x3300_0000, "rw", setvar_vaddr="net_buf"))
    for name, var, vaddr in [("bridge_rx_queue", "br_rx_queue", 0x3200_0000), ("bridge_rx_data", "br_rx_data", 0x3201_0000),
                             ("bridge_tx_queue", "br_tx_queue", 0x3202_0000), ("bridge_tx_data", "br_tx_data", 0x3203_0000)]:
        mr = MemoryRegion(sdf, name, BRIDGE_DATA_SIZE if name.endswith("data") else 0x1000)
        sdf.add_mr(mr)
        server.add_map(Map(mr, vaddr, "rw", setvar_vaddr=var))
    rtc = MemoryRegion(sdf, "rtc", 0x1000, paddr=RTC_PL031_PADDR)
    sdf.add_mr(rtc)
    server.add_map(Map(rtc, 0x3400_0000, "r", cached=False, setvar_vaddr="rtc_base"))

    for pd in (serial_driver, serial_virt_tx, serial_virt_rx, timer_driver, blk_driver, blk_virt, fs, server):
        sdf.add_pd(pd)
    sdf.add_channel(Channel(server, fs, a_id=SERVER_FS_CHANNEL_ID, pp_a=True))

    timer_system = Sddf.Timer(sdf, dtb.node(board.timer), timer_driver)
    timer_system.add_client(server)
    serial = Sddf.Serial(sdf, dtb.node(board.serial), serial_driver, serial_virt_tx, virt_rx=serial_virt_rx,
                         enable_color=False, begin_str="GNU/seL4 on ARM64 (no network or VM yet)\r\n")
    serial.add_client(server)
    blk = Sddf.Blk(sdf, dtb.node(board.blk), blk_driver, blk_virt)
    blk.add_client(fs, partition=0, data_size=FS_CACHE_SIZE)      # partition 1: GNU/seL4 ext2
    for system in (serial, timer_system, blk):
        assert system.connect()
        assert system.serialise_config(output_dir)
    # fs_ext2's block cache, mapped into posix_server too (one-copy reads and writes, as on x86)
    extra = '\n        <map mr="blk_client_fs_ext2_data" vaddr="0x40000000" perms="rw" setvar_vaddr="fs_cache" />'
    with open(f"{output_dir}/{sdf_file}", "w+") as f:
        f.write(server_cspace(sdf, extra))


def generate(sdf, sdf_file, output_dir):
    # ------------------------------------------------------------ console
    serial_virt_tx = ProtectionDomain("serial_virt_tx", "serial_virt_tx.elf", priority=199)
    serial_virt_rx = ProtectionDomain("serial_virt_rx", "serial_virt_rx.elf", priority=199, stack_size=0x2000)
    if CONSOLE == "graphical":
        serial_driver = ProtectionDomain("fbcon", "fbcon.elf", priority=200, stack_size=0x4000)
        fb = MemoryRegion(sdf, "fb", FB_SIZE, paddr=FB_PADDR)
        sdf.add_mr(fb)
        serial_driver.add_map(Map(fb, 0x4000_0000, "rw", setvar_vaddr="fb_base"))
        if FB_REGS_PADDR:
            fb_regs = MemoryRegion(sdf, "fb_regs", 0x1000, paddr=FB_REGS_PADDR)
            sdf.add_mr(fb_regs)
            serial_driver.add_map(Map(fb_regs, 0x5000_0000, "rw", cached=False, setvar_vaddr="fb_regs"))
        serial_driver.add_ioport(SystemDescription.IoPort(0x60, 1, 0))   # i8042: data
        serial_driver.add_ioport(SystemDescription.IoPort(0x64, 1, 1))   # i8042: status/commands
        serial_driver.add_irq(IrqIoapic(0, 1, 3, id=KBD_IRQ_CH))         # keyboard: IRQ 1
    else:
        serial_driver = ProtectionDomain("serial_driver", "serial_driver.elf", priority=200)
        serial_driver.add_ioport(SystemDescription.IoPort(0x3F8, 8, 0))  # COM1
        serial_driver.add_irq(IrqIoapic(0, 4, 0, id=1))

    # ------------------------------------------------------------ disk (none in a live system)
    if not LIVE:
        blk_driver = ProtectionDomain("blk_driver", "blk_driver.elf", priority=200, stack_size=0x2000)
        blk_virt = ProtectionDomain("blk_virt", "blk_virt.elf", priority=199, stack_size=0x2000)
        add_region(sdf, blk_driver, "virtio_requests", 0x10000, VIRTIO_BLK_REQUESTS_PADDR, 0x2020_0000, True)
        add_region(sdf, blk_driver, "virtio_metadata", 0x10000, VIRTIO_BLK_METADATA_PADDR, 0x2021_0000, True)
        add_region(sdf, blk_driver, "virtio_blk_regs", 0x4000, BLK_BAR, 0x6000_0000, False)
        blk_driver.add_irq(IrqIoapic(ioapic_id=0, pin=BLK_IRQ, vector=2, id=17,
                                     trigger=IrqIoapic.Trigger.LEVEL, polarity=IrqIoapic.Polarity.ACTIVELOW))

    # ------------------------------------------------------------ network (for the VM)
    eth_driver = ProtectionDomain("eth_driver", "net_null.elf" if LIVE else "eth_driver.elf",
                                  priority=195, budget=100, period=400)
    net_virt_tx = ProtectionDomain("net_virt_tx", "network_virt_tx.elf", priority=194, budget=20000)
    net_virt_rx = ProtectionDomain("net_virt_rx", "network_virt_rx.elf", priority=193)
    vm_net_copier = ProtectionDomain("vm_net_copier", "network_copy.elf", priority=192, budget=20000) if LINUX_VM else None
    if not LIVE:
        add_region(sdf, eth_driver, "hw_net_rings", 0x10000, VIRTIO_NET_VQUEUES_PADDR, 0x7000_0000, False)
        add_region(sdf, eth_driver, "virtio_net_regs", 0x4000, NET_BAR, 0x6000_0000, False)
        eth_driver.add_irq(IrqIoapic(ioapic_id=0, pin=NET_IRQ, vector=1, id=16,
                                     trigger=IrqIoapic.Trigger.LEVEL, polarity=IrqIoapic.Polarity.ACTIVELOW))

    # ------------------------------------------------------------ native GNU/seL4
    # Priorities: processes < posix_server < fs_ext2 < drivers (the PPC rule: the callee
    # has a higher priority than the caller)
    fs = ProtectionDomain("fs_ext2", "fs_ext2.elf", priority=160, stack_size=0x8000)
    server = ProtectionDomain("posix_server", "posix_server.elf", priority=150, stack_size=0x10000)
    server.add_ioport(SystemDescription.IoPort(0x70, 2, 0))       # CMOS clock (RTC_IOPORT in clock.c)
    if not LIVE:
        server.add_ioport(SystemDescription.IoPort(0x604, 2, 1))  # ACPI PM1a control (power off; q35)
    server.add_ioport(SystemDescription.IoPort(0xCF9, 1, 2))      # reset control register (reboot)

    fs_buf = MemoryRegion(sdf, "fs_buf", FS_BUF_SIZE)
    sdf.add_mr(fs_buf)
    server.add_map(Map(fs_buf, 0x3100_0000, "rw", setvar_vaddr="fs_buf"))
    fs.add_map(Map(fs_buf, 0x3100_0000, "rw", setvar_vaddr="fs_buf"))

    # Processes are not listed here: posix_server creates them on demand (server/mem.c)

    # ------------------------------------------------------------ VM with Docker
    # Priority below native processes: the shell stays responsive even when the VM is busy
    vmm = vmm_system = None
    if LINUX_VM:
        vmm = ProtectionDomain("CLIENT_VMM", "client_vmm.elf", priority=90, stack_size=0x4000)
        vm = VirtualMachine("linux", [VirtualMachine.Vcpu(id=0)])
        vmm_system = Vmm(sdf, vmm, vm, None)
        guest_ram = MemoryRegion(sdf, "guest_ram", GUEST_RAM_SIZE)
        sdf.add_mr(guest_ram)
        vmm.add_map(Map(guest_ram, GUEST_RAM_VMM_VADDR, "rw"))
        vm.add_map(Map(guest_ram, 0x0, "rwx"))

    # ------------------------------------------------------------ native networking: FreeBSD TCP/IP stack
    # Above posix_server (which calls it over PPC) and below the drivers
    netstack = ProtectionDomain("netstack", "netstack.elf", priority=155, stack_size=0x40000)
    netstack_copier = ProtectionDomain("netstack_net_copier", "netstack_copy.elf", priority=192, budget=20000)
    netstack_heap = MemoryRegion(sdf, "netstack_heap", NETSTACK_HEAP_SIZE)
    sdf.add_mr(netstack_heap)
    netstack.add_map(Map(netstack_heap, 0x20_0000_0000, "rw", setvar_vaddr="netstack_heap"))
    net_buf = MemoryRegion(sdf, "net_buf", NET_BUF_SIZE)
    sdf.add_mr(net_buf)
    server.add_map(Map(net_buf, 0x3300_0000, "rw", setvar_vaddr="net_buf"))
    netstack.add_map(Map(net_buf, 0x10_0000_0000, "rw", setvar_vaddr="net_buf"))

    timer_driver = ProtectionDomain("timer_driver", "timer_driver.elf", priority=254)
    timer_system = Sddf.Timer(sdf, None, timer_driver)
    if LINUX_VM:
        timer_system.add_client(vmm)
    timer_system.add_client(server)          # poll() and nanosleep() timeouts
    timer_system.add_client(netstack)        # TCP/IP stack clock
    add_x86_hpet(sdf, timer_driver)
    # Its heartbeat from the kernel's timer: up to 100 us every 1 ms, a few used (timer/ticker.c); the HPET alone
    # cannot be trusted to ring (on the laptop it almost stops while the CPU idles)
    ticker = ProtectionDomain("timer_ticker", "timer_ticker.elf", priority=253, budget=100, period=1000)
    timer_next = MemoryRegion(sdf, "timer_next", 0x1000)
    sdf.add_mr(timer_next)
    timer_driver.add_map(Map(timer_next, 0x5100_0000, "rw", setvar_vaddr="timer_next"))
    ticker.add_map(Map(timer_next, 0x5100_0000, "r", setvar_vaddr="timer_next"))

    for pd in (serial_driver, serial_virt_tx, serial_virt_rx, eth_driver, net_virt_tx, net_virt_rx,
               vm_net_copier, fs, server, vmm, timer_driver, ticker, netstack, netstack_copier):
        if pd:
            sdf.add_pd(pd)
    sdf.add_channel(Channel(ticker, timer_driver, a_id=0, b_id=60))     # TICK_CH in timer/hpet_timer.c
    if not LIVE:
        sdf.add_pd(blk_driver)
        sdf.add_pd(blk_virt)

    sdf.add_channel(Channel(server, fs, a_id=SERVER_FS_CHANNEL_ID, pp_a=True))
    sdf.add_channel(Channel(server, netstack, a_id=SERVER_NET_CHANNEL_ID, b_id=NETSTACK_SERVER_CHANNEL_ID, pp_a=True))
    sdf.add_channel(Channel(server, netstack, a_id=SERVER_LEASE_CHANNEL_ID, b_id=NETSTACK_LEASE_CHANNEL_ID))

    # ------------------------------------------------------------ GNU/seL4 <-> VM bridge
    # Two byte queues (sDDF serial format) that the VM sees as /dev/hvc1:
    #   rx: posix_server -> VM     tx: VM -> posix_server
    # Without the VM posix_server keeps its side mapped (bridge_init) but nobody is at the other end
    for name, server_var, vmm_var, vaddr in [
        ("bridge_rx_queue", "br_rx_queue", "bridge_rx_queue", 0x3200_0000),
        ("bridge_rx_data",  "br_rx_data",  "bridge_rx_data",  0x3201_0000),
        ("bridge_tx_queue", "br_tx_queue", "bridge_tx_queue", 0x3202_0000),
        ("bridge_tx_data",  "br_tx_data",  "bridge_tx_data",  0x3203_0000),
    ]:
        mr = MemoryRegion(sdf, name, BRIDGE_DATA_SIZE if name.endswith("data") else 0x1000)
        sdf.add_mr(mr)
        server.add_map(Map(mr, vaddr, "rw", setvar_vaddr=server_var))
        if LINUX_VM:
            vmm.add_map(Map(mr, 0x2_0000_0000 + vaddr, "rw", setvar_vaddr=vmm_var))
    if LINUX_VM:
        sdf.add_channel(Channel(server, vmm, a_id=SERVER_BRIDGE_CHANNEL_ID, b_id=VMM_BRIDGE_CHANNEL_ID))

    # ------------------------------------------------------------ sDDF subsystems
    serial = Sddf.Serial(sdf, None, serial_driver, serial_virt_tx, virt_rx=serial_virt_rx, enable_color=False,
                         begin_str="GNU/seL4: Ctrl-\\ 1 Enter -> VM console (Docker); Ctrl-\\ 0 Enter -> GNU/seL4\r\n"
                         if LINUX_VM else "GNU/seL4 (no Linux VM in this build)\r\n")
    serial.add_client(server)        # 0: GNU/seL4
    if LINUX_VM:
        serial.add_client(vmm)       # 1: VM

    systems = [serial, timer_system] + ([vmm_system] if LINUX_VM else [])
    if not LIVE:
        blk = Sddf.Blk(sdf, None, blk_driver, blk_virt)
        blk.add_client(fs, partition=0, data_size=FS_CACHE_SIZE)      # partition 1: GNU/seL4 ext2
        if LINUX_VM:
            blk.add_client(vmm, partition=1, data_size=VM_DISK_DATA_SIZE) # partition 2: the VM's /var/lib/docker
        systems.append(blk)

    net = Sddf.Net(sdf, None, eth_driver, net_virt_tx, net_virt_rx)
    # Fixed MACs (otherwise sdfgen makes one up on every build and DHCP hands out a new lease);
    # 02:... = locally administered
    if LINUX_VM:
        net.add_client_with_copier(vmm, vm_net_copier, mac_addr="02:5e:14:00:00:0a")
    net.add_client_with_copier(netstack, netstack_copier, mac_addr="02:5e:14:00:00:10")

    systems.append(net)
    for system in systems:
        assert system.connect()
        assert system.serialise_config(output_dir)

    # fs_ext2's block cache, mapped into posix_server too (sdfgen made the region): a file read is
    # copied from it straight to the program (FS_OP_READ_REF). The live system's cache is inside
    # fs_ext2 (a RAM disk), not this region
    extra = "" if LIVE else '\n        <map mr="blk_client_fs_ext2_data" vaddr="0x40000000" perms="rw" setvar_vaddr="fs_cache" />'
    with open(f"{output_dir}/{sdf_file}", "w+") as f:
        f.write(server_cspace(sdf, extra))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sddf", required=True)
    parser.add_argument("--board", required=True, choices=[b.name for b in BOARDS])
    parser.add_argument("--output", required=True)
    parser.add_argument("--sdf", required=True)
    parser.add_argument("--dtb", help="the machine's device tree (ARM)")
    args = parser.parse_args()

    board = next(filter(lambda b: b.name == args.board, BOARDS))
    if board.arch == SystemDescription.Arch.AARCH64:
        with open(args.dtb, "rb") as f:
            dtb = DeviceTree(f.read())
        sdf = SystemDescription(board.arch, board.paddr_top)
        Sddf(args.sddf)
        generate_arm(sdf, args.sdf, args.output, board, dtb)
        raise SystemExit(0)
    # As in libvmm/examples/virtio_pci: the physical regions sdfgen allocates on its own
    # go below this address, away from the fixed ones of the virtio drivers
    # The laptop has RAM only up to 0x3554_0000 in that area (its memory map), so lower there
    sdf = SystemDescription(board.arch, 0x3000_0000 if LIVE else 0x5000_0000)
    Sddf(args.sddf)
    generate(sdf, args.sdf, args.output)
