/* WebAssembly's atomics (a shared memory, -matomics) */
#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	__atomic_compare_exchange_n(p, &t, s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

#define a_barrier a_barrier
static inline void a_barrier()
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#define a_crash a_crash
static inline void a_crash()
{
	__builtin_trap();
}
