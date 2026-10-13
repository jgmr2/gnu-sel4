#include <stdint.h>
#include "libc.h"

/* The thread pointer (arch/wasm32/pthread_arch.h) */
hidden uintptr_t __wasm_tp;

int __set_thread_area(void *p)
{
	__wasm_tp = (uintptr_t)p;
	return 0;
}
