/*
 * posix_server VFS: network sockets (netstack) and the network configuration (see vfs.h).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vfs.h"

/* ---------------- sockets (FreeBSD TCP/IP stack in netstack) ---------------- */

/* Inside netstack sockets never block: here we decide whether the call waits
 * (POSIX_RETRY, retried when netstack reports activity) */
uintptr_t net_buf;                         /* shared with netstack (setvar in meta.py) */
static uint32_t net_waiters;


long net_call(long op, long a1, long a2, long a3, long a4)
{
    microkit_mr_set(0, op);
    microkit_mr_set(1, a1);
    microkit_mr_set(2, a2);
    microkit_mr_set(3, a3);
    microkit_mr_set(4, a4);
    microkit_ppcall(NET_CH, microkit_msginfo_new(0, 5));
    return (long)microkit_mr_get(0);
}

/* netstack notifies: whatever was waiting on a socket is retried */
void net_notified(void)
{
    proc_wake_mask(net_waiters);
    net_waiters = 0;
}

/* ---------------- network configuration (netstack's DHCP) ---------------- */

static const char *ip4(uint32_t a, char *out)       /* network byte order -> "a.b.c.d" */
{
    const uint8_t *b = (const uint8_t *)&a;
    sddf_snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return out;
}

static bool net_info(struct net_info *ni)
{
    if (net_call(NET_OP_INFO, 0, 0, 0, 0) < 0) return false;
    memcpy(ni, (void *)net_buf, sizeof *ni);
    return true;
}


/* New or renewed lease: the DNS servers the DHCP server gave go to /etc/resolv.conf */
void net_lease_changed(void)
{
    struct net_info ni;
    if (!net_info(&ni) || !ni.dns[0]) return;
    char buf[192], a[16], b[16];
    int n = sddf_snprintf(buf, sizeof buf, "# Written by posix_server from the network configuration (netcfg, DHCP)\n"
                          "nameserver %s\n", ip4(ni.dns[0], a));
    if (ni.dns[1]) n += sddf_snprintf(buf + n, sizeof buf - n, "nameserver %s\n", ip4(ni.dns[1], b));
    write_file("/etc/resolv.conf", buf, n);
}

/* /proc/net/dhcp */
long net_info_text(char *out, size_t max)
{
    static const char *const states[] = { "off", "init-reboot (asking for the previous address)",
                                          "selecting (looking for a server)",
                                          "requesting (asking for the offered address)", "bound", "renewing",
                                          "static (no DHCP)" };
    struct net_info ni;
    if (!net_info(&ni)) return sddf_snprintf(out, max, "netstack is not responding\n");
    char ip[16], gw[16], d0[16], d1[16], sv[16];
    int pl = __builtin_popcount(ni.mask);
    return sddf_snprintf(out, max,
        "state:     %s\ninterface: eth0 %02x:%02x:%02x:%02x:%02x:%02x\naddress:   %s/%d\ngateway:   %s\n"
        "dns:       %s %s\nserver:    %s\nlease:     %u s (%u s left)\n",
        ni.state < 7 ? states[ni.state] : "?", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5],
        ip4(ni.ip, ip), pl, ip4(ni.gw, gw), ip4(ni.dns[0], d0), ni.dns[1] ? ip4(ni.dns[1], d1) : "",
        ip4(ni.server, sv), ni.lease, ni.remaining);
}

static long sock_wait(struct ofile *f, long r, long flags)
{
    if (r != -EAGAIN) return r;
    if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
    net_waiters |= 1u << PROC_INDEX(cur);
    return POSIX_RETRY;
}

/* The address netstack returned -> the process's buffer (MR1 = length) */
void sock_addr_out(void)
{
    long alen = microkit_mr_get(1);
    if (alen > NET_ADDR_MAX) alen = NET_ADDR_MAX;
    memcpy((void *)(client_buf + NET_ADDR_OFF), (void *)(net_buf + NET_ADDR_OFF), alen);
    microkit_mr_set(1, alen);
}

long sock_socket(long domain, long type, long proto)
{
    int of = ofile_alloc(OF_SOCKET);
    if (of < 0) return -ENFILE;
    if (domain == AF_UNIX_) {
        ofiles[of].refs = 0;
        ofiles[of].type = OF_FREE;
        return usock_new(type);
    }
    long s = net_call(NET_OP_SOCKET, domain, type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC), proto, 0);
    if (s < 0) {
        ofiles[of].refs = 0;
        ofiles[of].type = OF_FREE;
        return s;
    }
    ofiles[of].ino = s;
    ofiles[of].flags = 2 | (type & SOCK_NONBLOCK ? O_NONBLOCK : 0);       /* O_RDWR */
    int fd = fd_alloc(of, 0, type & SOCK_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}

/* bind and connect: address in the process's buffer, at NET_ADDR_OFF */
long sock_addr_call(long op, struct ofile *f, long alen)
{
    if (alen < 0 || alen > NET_ADDR_MAX) return -EINVAL;
    memcpy((void *)(net_buf + NET_ADDR_OFF), (void *)(client_buf + NET_ADDR_OFF), alen);
    return net_call(op, f->ino, alen, 0, 0);
}

long sock_connect(struct ofile *f, long alen)
{
    if (f->off) {                                     /* blocking connect() already in progress */
        long p = net_call(NET_OP_POLL, f->ino, POLLOUT_, 0, 0);
        if (p >= 0 && !(p & (POLLOUT_ | POLLERR_ | POLLHUP_))) {
            net_waiters |= 1u << PROC_INDEX(cur);
            return POSIX_RETRY;
        }
        f->off = 0;
        long r = net_call(NET_OP_GETSOCKOPT, f->ino, SOL_SOCKET, SO_ERROR, 4);
        if (r < 0) return r;
        int err = *(int *)net_buf;
        return err ? -err : 0;
    }
    long r = sock_addr_call(NET_OP_CONNECT, f, alen);
    if (r != -EINPROGRESS) return r;
    if (f->flags & O_NONBLOCK) return r;
    f->off = 1;
    net_waiters |= 1u << PROC_INDEX(cur);
    return POSIX_RETRY;
}

long sock_accept(struct ofile *f, long flags)
{
    int of = ofile_alloc(OF_SOCKET);
    if (of < 0) return -ENFILE;
    long s = net_call(NET_OP_ACCEPT, f->ino, 0, 0, 0);
    if (s < 0) {
        ofiles[of].refs = 0;
        ofiles[of].type = OF_FREE;
        return sock_wait(f, s, 0);
    }
    sock_addr_out();
    ofiles[of].ino = s;
    ofiles[of].flags = 2 | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0);
    int fd = fd_alloc(of, 0, flags & SOCK_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}

/* send/sendto/write: data at the start of the buffer, address (alen > 0) at NET_ADDR_OFF */
long sock_send(struct ofile *f, size_t len, long flags, long alen)
{
    if (len > NET_DATA_MAX) len = NET_DATA_MAX;
    memcpy((void *)net_buf, (void *)client_buf, len);
    if (alen > 0) memcpy((void *)(net_buf + NET_ADDR_OFF), (void *)(client_buf + NET_ADDR_OFF), alen);
    long r = net_call(NET_OP_SEND, f->ino, len, flags, alen > 0 ? alen : 0);
    if (r == -EPIPE && !(flags & MSG_NOSIGNAL)) proc_signal(cur, SIGPIPE);
    return sock_wait(f, r, flags);
}

/* recv/recvfrom/read: data at the start of the buffer; source address at NET_ADDR_OFF (MR1) */
long sock_recv(struct ofile *f, size_t len, long flags)
{
    if (len > NET_DATA_MAX) len = NET_DATA_MAX;
    long r = net_call(NET_OP_RECV, f->ino, len, flags, 0);
    microkit_mr_set(2, 0);                              /* no descriptors (AF_UNIX only) */
    if (r < 0) return sock_wait(f, r, flags);
    memcpy((void *)client_buf, (void *)net_buf, r);
    sock_addr_out();
    return r;
}

short sock_poll(struct ofile *f, short events, bool wait)
{
    long p = net_call(NET_OP_POLL, f->ino, events, 0, 0);
    short r = p < 0 ? POLLERR : p;
    if (!(r & (events | POLLERR | POLLHUP)) && wait) net_waiters |= 1u << PROC_INDEX(cur);
    return r;
}

/* The socket calls on a network socket (vfs.c has checked the descriptor) */
long sock_syscall(long nr, struct ofile *f, long a2, long a3, long a4, long a5, long a6)
{
    (void)a6;
    switch (nr) {
    case __NR_bind:        return sock_addr_call(NET_OP_BIND, f, a3);
    case __NR_connect:     return sock_connect(f, a3);
    case __NR_listen:      return net_call(NET_OP_LISTEN, f->ino, a2, 0, 0);
    case __NR_accept:      return sock_accept(f, 0);
    case __NR_accept4:     return sock_accept(f, a4);
    case __NR_sendto:      return sock_send(f, a3, a4, a5 > NET_ADDR_MAX ? NET_ADDR_MAX : a5);
    case __NR_recvfrom:    return sock_recv(f, a3, a4);
    case __NR_shutdown:    return net_call(NET_OP_SHUTDOWN, f->ino, a2, 0, 0);
    case __NR_setsockopt:
        if (a5 > NET_DATA_MAX) return -EINVAL;
        memcpy((void *)net_buf, (void *)client_buf, a5);
        return net_call(NET_OP_SETSOCKOPT, f->ino, a2, a3, a5);
    case __NR_getsockopt: {
        long r = net_call(NET_OP_GETSOCKOPT, f->ino, a2, a3, a4);
        if (r >= 0) {
            long olen = microkit_mr_get(1);
            memcpy((void *)client_buf, (void *)net_buf, olen);
            microkit_mr_set(1, olen);
        }
        return r;
    }
    case __NR_getsockname:
    case __NR_getpeername: {
        long r = net_call(nr == __NR_getsockname ? NET_OP_GETSOCKNAME : NET_OP_GETPEERNAME, f->ino, 0, 0, 0);
        if (r >= 0) sock_addr_out();
        return r;
    }
    }
    return -ENOSYS;
}
