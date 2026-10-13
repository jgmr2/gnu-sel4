/*
 * What posix_server does differently on each architecture (TODO item 16): the seL4 objects of an
 * address space and how frames are mapped into it, the registers a thread starts with, the
 * counter that keeps time, and keeping the instruction cache right after writing code (ARM).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <sel4/sel4.h>

#if defined(__wasm__)
/* The browser port (wasmkit, TODO item 24): no frames or threads of seL4's here; the clock is the
 * page's, in nanoseconds; random bytes from the page's crypto.getRandomValues */
#define ARCH_UNAME          "wasm32"
uint64_t wasmkit_now_ns(void);
__attribute__((import_module("env"), import_name("wk_random"))) uint32_t wk_random(void);
static inline uint64_t arch_counter(void) { return wasmkit_now_ns(); }
static inline uint64_t arch_counter_hz(void) { return 1000000000; }
static inline bool arch_hwrand(uint64_t *v) { *v = (uint64_t)wk_random() << 32 | wk_random(); return true; }

#elif defined(__x86_64__)
#include <sel4/sel4_arch/mapping.h>      /* SEL4_MAPPING_LOOKUP_*, seL4_MappingFailedLookupLevel */

#define ARCH_FRAME_OBJECT   seL4_X86_4K
#define ARCH_VSPACE_OBJECT  seL4_X64_PML4Object
#define ARCH_ELF_MACHINE    62                  /* EM_X86_64 */
#define ARCH_UNAME          "x86_64"            /* uname -m */

static inline seL4_Error arch_page_map(seL4_CPtr frame, seL4_CPtr vspace, uintptr_t vaddr, seL4_CapRights_t rights)
{
    return seL4_X86_Page_Map(frame, vspace, vaddr, rights, seL4_X86_Default_VMAttributes);
}
static inline seL4_Error arch_page_unmap(seL4_CPtr frame) { return seL4_X86_Page_Unmap(frame); }
static inline seL4_Error arch_asid_assign(seL4_CPtr pool, seL4_CPtr vspace) { return seL4_X86_ASIDPool_Assign(pool, vspace); }

/* The paging structure a failed mapping is missing (seL4_MappingFailedLookupLevel), mapped at
 * vaddr; make() retypes an object of a type */
static inline bool arch_add_table(seL4_CPtr (*make)(seL4_Word, seL4_Word), seL4_CPtr vspace, uintptr_t vaddr)
{
    seL4_Word type;
    switch (seL4_MappingFailedLookupLevel()) {
    case SEL4_MAPPING_LOOKUP_NO_PDPT: type = seL4_X86_PDPTObject; break;
    case SEL4_MAPPING_LOOKUP_NO_PD:   type = seL4_X86_PageDirectoryObject; break;
    case SEL4_MAPPING_LOOKUP_NO_PT:   type = seL4_X86_PageTableObject; break;
    default: return false;
    }
    seL4_CPtr pt = make(type, 0);
    if (!pt) return false;
    seL4_X86_VMAttributes a = seL4_X86_Default_VMAttributes;
    return !(type == seL4_X86_PDPTObject ? seL4_X86_PDPT_Map(pt, vspace, vaddr, a)
           : type == seL4_X86_PageDirectoryObject ? seL4_X86_PageDirectory_Map(pt, vspace, vaddr, a)
           : seL4_X86_PageTable_Map(pt, vspace, vaddr, a));
}

/* x86 keeps instruction fetches coherent with stores by itself */
static inline void arch_sync_icache(seL4_CPtr frame) { (void)frame; }

static inline void arch_ctx_start(seL4_UserContext *c, uintptr_t pc, uintptr_t sp, uintptr_t tp)
{
    c->rip = pc;
    c->rsp = sp;
    c->fs_base = tp;
}
/* A stack pointer for a function entered as if called (x86: the return address's slot) */
static inline uintptr_t arch_call_sp(uintptr_t sp) { return (sp & ~15UL) - 8; }

static inline uint64_t arch_counter(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static inline bool arch_hwrand(uint64_t *v)
{
    unsigned long long x;
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(x), "=qm"(ok));
    *v = x;
    return ok;
}

#elif defined(__aarch64__)

#define ARCH_FRAME_OBJECT   seL4_ARM_SmallPageObject
#define ARCH_VSPACE_OBJECT  seL4_ARM_VSpaceObject
#define ARCH_ELF_MACHINE    183                 /* EM_AARCH64 */
#define ARCH_UNAME          "aarch64"

static inline seL4_Error arch_page_map(seL4_CPtr frame, seL4_CPtr vspace, uintptr_t vaddr, seL4_CapRights_t rights)
{
    return seL4_ARM_Page_Map(frame, vspace, vaddr, rights, seL4_ARM_Default_VMAttributes);
}
static inline seL4_Error arch_page_unmap(seL4_CPtr frame) { return seL4_ARM_Page_Unmap(frame); }
static inline seL4_Error arch_asid_assign(seL4_CPtr pool, seL4_CPtr vspace) { return seL4_ARM_ASIDPool_Assign(pool, vspace); }

/* aarch64 has one kind of page table for every level below the VSpace */
static inline bool arch_add_table(seL4_CPtr (*make)(seL4_Word, seL4_Word), seL4_CPtr vspace, uintptr_t vaddr)
{
    seL4_CPtr pt = make(seL4_ARM_PageTableObject, 0);
    return pt && !seL4_ARM_PageTable_Map(pt, vspace, vaddr, seL4_ARM_Default_VMAttributes);
}

/* Code the server wrote into a process's frame (a program loaded, a fork's copy): its data
 * cache cleaned and the instruction cache invalidated, through the frame's mapping */
static inline void arch_sync_icache(seL4_CPtr frame) { seL4_ARM_Page_Unify_Instruction(frame, 0, 4096); }

static inline void arch_ctx_start(seL4_UserContext *c, uintptr_t pc, uintptr_t sp, uintptr_t tp)
{
    c->pc = pc;
    c->sp = sp;
    c->tpidr_el0 = tp;
}
static inline uintptr_t arch_call_sp(uintptr_t sp) { return sp & ~15UL; }

/* The physical counter (seL4 exports it to user mode: CONFIG_EXPORT_PCNT_USER) */
static inline uint64_t arch_counter(void)
{
    uint64_t c;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(c));
    return c;
}
static inline uint64_t arch_counter_hz(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

static inline bool arch_hwrand(uint64_t *v) { (void)v; return false; }   /* TODO: virtio-rng */

#else
#error "server/arch.h: unsupported architecture"
#endif
