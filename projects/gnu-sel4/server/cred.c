/*
 * Process credentials: real, effective and saved user and group IDs and the supplementary
 * groups, with the POSIX/Linux rules for changing them; and the permission check every file
 * access goes through.
 *
 * Only a process with effective uid 0 is privileged (no capabilities yet). File system user
 * and group IDs (setfsuid) follow the effective ones, as they do in practice.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <os/sddf.h>
#include <string.h>
#include "server.h"
#include "syscall_nr.h"

#define NO_CHANGE ((uint32_t)-1)

void cred_init_root(struct proc *p)
{
    p->uid = p->euid = p->suid = 0;
    p->gid = p->egid = p->sgid = 0;
    p->ngroups = 0;
}

void cred_copy(struct proc *to, const struct proc *from)
{
    to->uid = from->uid; to->euid = from->euid; to->suid = from->suid;
    to->gid = from->gid; to->egid = from->egid; to->sgid = from->sgid;
    to->ngroups = from->ngroups;
    memcpy(to->groups, from->groups, sizeof to->groups);
}

/* exec of a set-user-ID / set-group-ID program; the saved IDs follow the effective ones */
void cred_exec(struct proc *p, uint32_t mode, uint32_t file_uid, uint32_t file_gid)
{
    if (mode & 04000) p->euid = file_uid;
    if (mode & 02000) p->egid = file_gid;
    p->suid = p->euid;
    p->sgid = p->egid;
}

bool cred_in_group(const struct proc *p, uint32_t gid)
{
    if (p->egid == gid) return true;
    for (int i = 0; i < p->ngroups; i++) if (p->groups[i] == gid) return true;
    return false;
}

/* May p access a file with this mode and owner? want: 4 read, 2 write, 1 execute/search.
 * Root may do anything, except execute a file that has no x bit at all. */
static bool may(uint32_t who, uint32_t who_gid, const struct proc *p, uint32_t mode, uint32_t uid, uint32_t gid, int want)
{
    if (who == 0) return !(want & 1) || (mode & 0111) || (mode & 0170000) == 0040000;
    bool member = who_gid == gid;
    for (int i = 0; i < p->ngroups && !member; i++) member = p->groups[i] == gid;
    uint32_t bits = who == uid ? mode >> 6 : member ? mode >> 3 : mode;
    return (bits & want & 7) == (uint32_t)want;
}

bool cred_may(const struct proc *p, uint32_t mode, uint32_t uid, uint32_t gid, int want)
{
    return may(p->euid, p->egid, p, mode, uid, gid, want);
}

/* access(2): with the real IDs, so a set-user-ID program can ask what its user may do */
bool cred_may_real(const struct proc *p, uint32_t mode, uint32_t uid, uint32_t gid, int want)
{
    return may(p->uid, p->gid, p, mode, uid, gid, want);
}

/* May p signal q? (as Linux: privileged, or the real or effective uid of the sender equals the
 * real or saved uid of the target) */
bool cred_may_signal(const struct proc *p, const struct proc *q)
{
    return p->euid == 0 || p->uid == q->uid || p->uid == q->suid || p->euid == q->uid || p->euid == q->suid;
}

static bool one_of(uint32_t v, uint32_t a, uint32_t b, uint32_t c) { return v == a || v == b || v == c; }

/* setresuid/setresgid: the general form the others are written in */
static long set_res(uint32_t *r, uint32_t *e, uint32_t *s, uint32_t nr, uint32_t ne, uint32_t ns, bool priv)
{
    if (!priv && ((nr != NO_CHANGE && !one_of(nr, *r, *e, *s)) || (ne != NO_CHANGE && !one_of(ne, *r, *e, *s)) ||
                  (ns != NO_CHANGE && !one_of(ns, *r, *e, *s))))
        return -EPERM;
    if (nr != NO_CHANGE) *r = nr;
    if (ne != NO_CHANGE) *e = ne;
    if (ns != NO_CHANGE) *s = ns;
    return 0;
}

bool cred_handles(long nr)
{
    switch (nr) {
    case __NR_getuid: case __NR_geteuid: case __NR_getgid: case __NR_getegid:
    case __NR_setuid: case __NR_setgid: case __NR_setreuid: case __NR_setregid:
    case __NR_setresuid: case __NR_setresgid: case __NR_getresuid: case __NR_getresgid:
    case __NR_getgroups: case __NR_setgroups: case __NR_setfsuid: case __NR_setfsgid:
        return true;
    }
    return false;
}

long cred_syscall(long nr, int64_t a1, int64_t a2, int64_t a3)
{
    struct proc *p = cur;
    bool priv = p->euid == 0;
    uint32_t x1 = a1, x2 = a2, x3 = a3;
    switch (nr) {
    case __NR_getuid:  return p->uid;
    case __NR_geteuid: return p->euid;
    case __NR_getgid:  return p->gid;
    case __NR_getegid: return p->egid;
    case __NR_setuid:                                   /* privileged: all three; else only euid */
        if (priv) { p->uid = p->euid = p->suid = x1; return 0; }
        return set_res(&p->uid, &p->euid, &p->suid, NO_CHANGE, x1, NO_CHANGE, false);
    case __NR_setgid:
        if (priv) { p->gid = p->egid = p->sgid = x1; return 0; }
        return set_res(&p->gid, &p->egid, &p->sgid, NO_CHANGE, x1, NO_CHANGE, false);
    case __NR_setreuid: {                               /* the saved uid follows a new effective one */
        if (!priv && ((x1 != NO_CHANGE && x1 != p->uid && x1 != p->euid) ||
                      (x2 != NO_CHANGE && !one_of(x2, p->uid, p->euid, p->suid))))
            return -EPERM;
        uint32_t olduid = p->uid;
        if (x1 != NO_CHANGE) p->uid = x1;
        if (x2 != NO_CHANGE) p->euid = x2;
        if (x1 != NO_CHANGE || (x2 != NO_CHANGE && x2 != olduid)) p->suid = p->euid;
        return 0;
    }
    case __NR_setregid: {
        if (!priv && ((x1 != NO_CHANGE && x1 != p->gid && x1 != p->egid) ||
                      (x2 != NO_CHANGE && !one_of(x2, p->gid, p->egid, p->sgid))))
            return -EPERM;
        uint32_t oldgid = p->gid;
        if (x1 != NO_CHANGE) p->gid = x1;
        if (x2 != NO_CHANGE) p->egid = x2;
        if (x1 != NO_CHANGE || (x2 != NO_CHANGE && x2 != oldgid)) p->sgid = p->egid;
        return 0;
    }
    case __NR_setresuid: return set_res(&p->uid, &p->euid, &p->suid, x1, x2, x3, priv);
    case __NR_setresgid: return set_res(&p->gid, &p->egid, &p->sgid, x1, x2, x3, priv);
    case __NR_getresuid:                                /* -> MR1..MR3 (the runtime fills the pointers) */
        microkit_mr_set(1, p->uid); microkit_mr_set(2, p->euid); microkit_mr_set(3, p->suid);
        return 0;
    case __NR_getresgid:
        microkit_mr_set(1, p->gid); microkit_mr_set(2, p->egid); microkit_mr_set(3, p->sgid);
        return 0;
    case __NR_getgroups: {                              /* list -> client_buf */
        if (a1 < 0) return -EINVAL;
        if (a1 == 0) return p->ngroups;
        if (a1 < p->ngroups) return -EINVAL;
        memcpy((void *)client_buf, p->groups, p->ngroups * sizeof(uint32_t));
        return p->ngroups;
    }
    case __NR_setgroups:                                /* list in client_buf */
        if (!priv) return -EPERM;
        if (a1 < 0 || a1 > NGROUPS_) return -EINVAL;
        memcpy(p->groups, (void *)client_buf, a1 * sizeof(uint32_t));
        p->ngroups = a1;
        return 0;
    case __NR_setfsuid: return p->euid;                 /* follows euid: report it, change nothing */
    case __NR_setfsgid: return p->egid;
    }
    return -ENOSYS;
}
