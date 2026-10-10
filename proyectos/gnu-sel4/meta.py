# Genera la descripción del sistema GNU/seL4 (gnu-sel4.system) y la configuración de sDDF.
#
#                       ┌─ posix_server ── procesos (creados a demanda con su untyped)
#   consola (COM1) ─────┤      │ PPC
#   serial_virt_rx/tx   │   fs_ext2 ──┐
#                       └─ CLIENT_VMM ── VM Linux (Docker)            disco (virtio-blk)
#                              │    └──────────────────────────────── blk_virt: partición 1 = ext2,
#                              │                                                 partición 2 = Docker
#   red (virtio-net) ── net_virt_rx/tx ── network_copy ── CLIENT_VMM
#
# Puente: posix_server <-> CLIENT_VMM (colas propias, /dev/hvc1 en la VM) para ejecutar
# comandos de Linux desde GNU/seL4 (/bin/linux, /bin/docker), como el interop de WSL.
#
# Consola: dos clientes del virtualizador serial. Ctrl-\ y luego el número y Enter cambia
# a quién va el teclado: 0 = GNU/seL4 (dash), 1 = la VM.
#
# Uso: meta.py --sddf <sddf> --board x86_64_generic_vtx --output <dir> --sdf gnu-sel4.system
import argparse
import os
import re
from sdfgen import SystemDescription, Sddf, Vmm
from board import BOARDS, add_x86_hpet

ProtectionDomain = SystemDescription.ProtectionDomain
VirtualMachine = SystemDescription.VirtualMachine
MemoryRegion = SystemDescription.MemoryRegion
Map = SystemDescription.Map
Channel = SystemDescription.Channel
IrqIoapic = SystemDescription.IrqIoapic

# ---- GNU/seL4 nativo (deben coincidir con include/sel4posix/*.h y server/server.h) ----
PROC_SLOTS = 32                       # PROC_SLOTS en procmem.h: scheduling contexts del lote
SERVER_FS_CHANNEL_ID = 21             # FS_CH
SERVER_BRIDGE_CHANNEL_ID = 22         # BRIDGE_CH en server/server.h
SERVER_NET_CHANNEL_ID = 23            # NET_CH en server/server.h
NETSTACK_SERVER_CHANNEL_ID = 50       # SERVER_CH en net/netstack.c
SERVER_LEASE_CHANNEL_ID = 24          # NET_LEASE_CH en server/server.h (aviso de concesión DHCP)
NETSTACK_LEASE_CHANNEL_ID = 51        # LEASE_CH en net/netstack.c
NET_BUF_SIZE = 0x1_0000               # NET_BUF_SIZE en include/sel4posix/net_protocol.h
VMM_BRIDGE_CHANNEL_ID = 50            # BRIDGE_CH en vm/guest_arch_init.h
BRIDGE_DATA_SIZE = 0x1_0000           # BR_DATA_SIZE / BRIDGE_DATA_SIZE
FS_BUF_SIZE = 0x1_0000
FS_CACHE_SIZE = 0x40_0000             # 4 MiB: región de datos de sDDF que fs_ext2 usa como caché

# Presupuesto de RAM: seL4 entrega la memoria de arranque como untyped "sucia" y la pone a cero
# ENTERA la primera vez que se usa, en bloques de potencia de 2. En q35 la RAM baja es un bloque
# de 2 GiB y la alta empieza con otro de 2-4 GiB: si todo (memoria de procesos + VM + imágenes)
# no cabe en la RAM baja (~1.85 GiB libres), se toca también el bloque alto y QEMU pasa de ~2.2
# a 4.3 GB o más. Hoy: procesos 768 MiB + VM 768 MiB + netstack 64 MiB + ~50 MiB de drivers.
NETSTACK_HEAP_SIZE = 0x400_0000       # NETSTACK_HEAP_SIZE en net/netstack.h

# CSpace raíz de posix_server para el gestor de memoria (= SRV_* en procmem.h). Elementos
# <cspace> de nuestro Microkit (third_party/microkit, rama gnu-sel4): sdfgen no los conoce.
SRV_ROOT_CNODE_SLOT = 1                # su propia CNode raíz: crea CNodes nuevas en 20..63
SRV_POOL_SLOT = 2                      # untypeds de la memoria de procesos (potencias de 2)
SRV_POOL_SIZES = [0x2000_0000, 0x1000_0000]                  # 512 + 256 MiB
SRV_ASID_POOL_SLOT = 5
SRV_TCB_SLOT = 6
SRV_EP_SLOT = 7
SRV_SCHED_POOL_SLOT = 8

# ---- VM (deben coincidir con vm/guest_arch_init.c) ----
GUEST_RAM_SIZE = 0x3000_0000          # 768 MiB
GUEST_RAM_VMM_VADDR = 0x1_0000_0000
VM_DISK_DATA_SIZE = 0x100_0000        # 16 MiB de búfer de disco para la VM

# Consola: "grafica" (pantalla + teclado PS/2, console/fbcon.c) o "serie" (UART COM1).
# Las dos son el "driver serial" de sDDF: el resto del sistema no cambia. make CONSOLA=serie
CONSOLA = os.environ.get("CONSOLA", "grafica")
# Pantalla bochs-display en la ranura 5 (make run): BAR0 framebuffer, BAR2 registros
FB_PADDR, FB_SIZE = 0xFD00_0000, 0x40_0000            # 1280x800x32 cabe en 4 MiB
FB_REGS_PADDR = 0xFEBC_A000
KBD_IRQ_CH = 10                       # KBD_IRQ_CH en console/fbcon.c

# ---- Dispositivos de QEMU q35 (ranuras PCI fijas: el firmware asigna estas BAR e IRQ) ----
# Con más de 4 GiB de RAM (make run usa 6 GiB) el firmware pone las BAR de 64 bits en la
# ventana alta 0x7000000000, siempre en este orden:
#   disco virtio-blk en la ranura 3: BAR4 0x7000000000, IRQ 11
#   red   virtio-net en la ranura 4: BAR4 0x7000004000, IRQ 10
BLK_BAR, BLK_IRQ = 0x70_0000_0000, 11
NET_BAR, NET_IRQ = 0x70_0000_4000, 10
# Colas de los drivers virtio de sDDF (direcciones fijas en los drivers)
VIRTIO_BLK_REQUESTS_PADDR = 0x5FDF_0000
VIRTIO_BLK_METADATA_PADDR = 0x5FFF_0000
VIRTIO_NET_VQUEUES_PADDR = 0x7A00_0000


def add_region(sdf, pd, name, size, paddr, vaddr, cached):
    mr = MemoryRegion(sdf, name, size, paddr=paddr)
    sdf.add_mr(mr)
    pd.add_map(Map(mr, vaddr, "rw", cached=cached))


def generate(sdf, sdf_file, output_dir):
    # ------------------------------------------------------------ consola
    serial_virt_tx = ProtectionDomain("serial_virt_tx", "serial_virt_tx.elf", priority=199)
    serial_virt_rx = ProtectionDomain("serial_virt_rx", "serial_virt_rx.elf", priority=199, stack_size=0x2000)
    if CONSOLA == "grafica":
        serial_driver = ProtectionDomain("fbcon", "fbcon.elf", priority=200, stack_size=0x4000)
        fb = MemoryRegion(sdf, "fb", FB_SIZE, paddr=FB_PADDR)
        sdf.add_mr(fb)
        serial_driver.add_map(Map(fb, 0x4000_0000, "rw", setvar_vaddr="fb_base"))
        fb_regs = MemoryRegion(sdf, "fb_regs", 0x1000, paddr=FB_REGS_PADDR)
        sdf.add_mr(fb_regs)
        serial_driver.add_map(Map(fb_regs, 0x5000_0000, "rw", cached=False, setvar_vaddr="fb_regs"))
        serial_driver.add_ioport(SystemDescription.IoPort(0x60, 1, 0))   # i8042: datos
        serial_driver.add_ioport(SystemDescription.IoPort(0x64, 1, 1))   # i8042: estado/órdenes
        serial_driver.add_irq(IrqIoapic(0, 1, 3, id=KBD_IRQ_CH))         # teclado: IRQ 1
    else:
        serial_driver = ProtectionDomain("serial_driver", "serial_driver.elf", priority=200)
        serial_driver.add_ioport(SystemDescription.IoPort(0x3F8, 8, 0))  # COM1
        serial_driver.add_irq(IrqIoapic(0, 4, 0, id=1))

    # ------------------------------------------------------------ disco
    blk_driver = ProtectionDomain("blk_driver", "blk_driver.elf", priority=200, stack_size=0x2000)
    blk_virt = ProtectionDomain("blk_virt", "blk_virt.elf", priority=199, stack_size=0x2000)
    add_region(sdf, blk_driver, "virtio_requests", 0x10000, VIRTIO_BLK_REQUESTS_PADDR, 0x2020_0000, True)
    add_region(sdf, blk_driver, "virtio_metadata", 0x10000, VIRTIO_BLK_METADATA_PADDR, 0x2021_0000, True)
    add_region(sdf, blk_driver, "virtio_blk_regs", 0x4000, BLK_BAR, 0x6000_0000, False)
    blk_driver.add_irq(IrqIoapic(ioapic_id=0, pin=BLK_IRQ, vector=2, id=17,
                                 trigger=IrqIoapic.Trigger.LEVEL, polarity=IrqIoapic.Polarity.ACTIVELOW))

    # ------------------------------------------------------------ red (para la VM)
    eth_driver = ProtectionDomain("eth_driver", "eth_driver.elf", priority=195, budget=100, period=400)
    net_virt_tx = ProtectionDomain("net_virt_tx", "network_virt_tx.elf", priority=194, budget=20000)
    net_virt_rx = ProtectionDomain("net_virt_rx", "network_virt_rx.elf", priority=193)
    vm_net_copier = ProtectionDomain("vm_net_copier", "network_copy.elf", priority=192, budget=20000)
    add_region(sdf, eth_driver, "hw_net_rings", 0x10000, VIRTIO_NET_VQUEUES_PADDR, 0x7000_0000, False)
    add_region(sdf, eth_driver, "virtio_net_regs", 0x4000, NET_BAR, 0x6000_0000, False)
    eth_driver.add_irq(IrqIoapic(ioapic_id=0, pin=NET_IRQ, vector=1, id=16,
                                 trigger=IrqIoapic.Trigger.LEVEL, polarity=IrqIoapic.Polarity.ACTIVELOW))

    # ------------------------------------------------------------ GNU/seL4 nativo
    # Prioridades: procesos < posix_server < fs_ext2 < drivers (regla de las PPC:
    # quien recibe la llamada tiene más prioridad que quien la hace)
    fs = ProtectionDomain("fs_ext2", "fs_ext2.elf", priority=160, stack_size=0x8000)
    server = ProtectionDomain("posix_server", "posix_server.elf", priority=150, stack_size=0x10000)
    server.add_ioport(SystemDescription.IoPort(0x70, 2, 0))       # reloj CMOS (RTC_IOPORT en clock.c)

    fs_buf = MemoryRegion(sdf, "fs_buf", FS_BUF_SIZE)
    sdf.add_mr(fs_buf)
    server.add_map(Map(fs_buf, 0x3100_0000, "rw", setvar_vaddr="fs_buf"))
    fs.add_map(Map(fs_buf, 0x3100_0000, "rw", setvar_vaddr="fs_buf"))

    # Los procesos no están aquí: posix_server los crea a demanda (server/mem.c)

    # ------------------------------------------------------------ VM con Docker
    # Prioridad por debajo de los procesos nativos: la shell responde aunque la VM esté ocupada
    vmm = ProtectionDomain("CLIENT_VMM", "client_vmm.elf", priority=90, stack_size=0x4000)
    vm = VirtualMachine("linux", [VirtualMachine.Vcpu(id=0)])
    vmm_system = Vmm(sdf, vmm, vm, None)
    guest_ram = MemoryRegion(sdf, "guest_ram", GUEST_RAM_SIZE)
    sdf.add_mr(guest_ram)
    vmm.add_map(Map(guest_ram, GUEST_RAM_VMM_VADDR, "rw"))
    vm.add_map(Map(guest_ram, 0x0, "rwx"))

    # ------------------------------------------------------------ red nativa: pila TCP/IP de FreeBSD
    # Por encima de posix_server (que la llamará por PPC) y por debajo de los drivers
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
    timer_system.add_client(vmm)
    timer_system.add_client(server)          # plazos de poll() y nanosleep()
    timer_system.add_client(netstack)        # reloj de la pila TCP/IP
    add_x86_hpet(sdf, timer_driver)

    for pd in (serial_driver, serial_virt_tx, serial_virt_rx, blk_driver, blk_virt,
               eth_driver, net_virt_tx, net_virt_rx, vm_net_copier, fs, server, vmm, timer_driver,
               netstack, netstack_copier):
        sdf.add_pd(pd)

    sdf.add_channel(Channel(server, fs, a_id=SERVER_FS_CHANNEL_ID, pp_a=True))
    sdf.add_channel(Channel(server, netstack, a_id=SERVER_NET_CHANNEL_ID, b_id=NETSTACK_SERVER_CHANNEL_ID, pp_a=True))
    sdf.add_channel(Channel(server, netstack, a_id=SERVER_LEASE_CHANNEL_ID, b_id=NETSTACK_LEASE_CHANNEL_ID))

    # ------------------------------------------------------------ puente GNU/seL4 <-> VM
    # Dos colas de bytes (formato sDDF serial) que la VM ve como /dev/hvc1:
    #   rx: posix_server -> VM     tx: VM -> posix_server
    for name, server_var, vmm_var, vaddr in [
        ("bridge_rx_queue", "br_rx_queue", "bridge_rx_queue", 0x3200_0000),
        ("bridge_rx_data",  "br_rx_data",  "bridge_rx_data",  0x3201_0000),
        ("bridge_tx_queue", "br_tx_queue", "bridge_tx_queue", 0x3202_0000),
        ("bridge_tx_data",  "br_tx_data",  "bridge_tx_data",  0x3203_0000),
    ]:
        mr = MemoryRegion(sdf, name, BRIDGE_DATA_SIZE if name.endswith("data") else 0x1000)
        sdf.add_mr(mr)
        server.add_map(Map(mr, vaddr, "rw", setvar_vaddr=server_var))
        vmm.add_map(Map(mr, 0x2_0000_0000 + vaddr, "rw", setvar_vaddr=vmm_var))
    sdf.add_channel(Channel(server, vmm, a_id=SERVER_BRIDGE_CHANNEL_ID, b_id=VMM_BRIDGE_CHANNEL_ID))

    # ------------------------------------------------------------ subsistemas sDDF
    serial = Sddf.Serial(sdf, None, serial_driver, serial_virt_tx, virt_rx=serial_virt_rx, enable_color=False,
                         begin_str="GNU/seL4: Ctrl-\\ 1 Enter -> consola de la VM (Docker); Ctrl-\\ 0 Enter -> dash\r\n")
    serial.add_client(server)        # 0: GNU/seL4
    serial.add_client(vmm)           # 1: VM

    blk = Sddf.Blk(sdf, None, blk_driver, blk_virt)
    blk.add_client(fs, partition=0, data_size=FS_CACHE_SIZE)          # partición 1: ext2 de GNU/seL4
    blk.add_client(vmm, partition=1, data_size=VM_DISK_DATA_SIZE)     # partición 2: /var/lib/docker de la VM

    net = Sddf.Net(sdf, None, eth_driver, net_virt_tx, net_virt_rx)
    # MAC fijas (si no, sdfgen inventa una en cada compilación y el DHCP da otra concesión);
    # 02:... = administradas localmente
    net.add_client_with_copier(vmm, vm_net_copier, mac_addr="02:5e:14:00:00:0a")
    net.add_client_with_copier(netstack, netstack_copier, mac_addr="02:5e:14:00:00:10")

    for system in (serial, blk, net, timer_system, vmm_system):
        assert system.connect()
        assert system.serialise_config(output_dir)

    caps = [f'<cap_cnode slot="{SRV_ROOT_CNODE_SLOT}" pd="posix_server" />']
    caps += [f'<cap_untyped slot="{SRV_POOL_SLOT + i}" size="{size:#x}" />' for i, size in enumerate(SRV_POOL_SIZES)]
    caps += [f'<cap_asid_pool slot="{SRV_ASID_POOL_SLOT}" />',
             f'<cap_tcb slot="{SRV_TCB_SLOT}" pd="posix_server" />',
             f'<cap_endpoint slot="{SRV_EP_SLOT}" pd="posix_server" />',
             f'<cap_sched_pool slot="{SRV_SCHED_POOL_SLOT}" count="{PROC_SLOTS}" />']
    xml, n = re.subn(r'(<protection_domain name="posix_server"[^>]*>)',
                     lambda m: m.group(1) + "\n        <cspace>" + "".join(caps) + "</cspace>", sdf.render())
    assert n == 1
    with open(f"{output_dir}/{sdf_file}", "w+") as f:
        f.write(xml)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sddf", required=True)
    parser.add_argument("--board", required=True, choices=[b.name for b in BOARDS])
    parser.add_argument("--output", required=True)
    parser.add_argument("--sdf", required=True)
    args = parser.parse_args()

    board = next(filter(lambda b: b.name == args.board, BOARDS))
    # Como en libvmm/examples/virtio_pci: las regiones físicas que sdfgen asigna por su
    # cuenta quedan bajo esta dirección, lejos de las fijas de los drivers virtio
    sdf = SystemDescription(board.arch, 0x5000_0000)
    Sddf(args.sddf)
    generate(sdf, args.sdf, args.output)
