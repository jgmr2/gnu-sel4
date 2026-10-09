/* netstack: pila TCP/IP de FreeBSD (F-Stack) como componente de seL4 */
#pragma once
#include <stdint.h>

#define NETSTACK_HEAP_SIZE 0x4000000UL          /* 64 MiB (= meta.py) */

void netstack_host_init(void);
uint64_t netstack_now_ns(void);
void netstack_log(const char *fmt, ...);
void panic(const char *fmt, ...) __attribute__((__noreturn__));
