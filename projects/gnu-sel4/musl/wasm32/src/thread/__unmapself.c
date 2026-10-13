#include "pthread_impl.h"

/* A thread unmapping its own stack: there are no threads on wasm32 yet */
_Noreturn void __unmapself(void *base, size_t size)
{
	__builtin_trap();
}
