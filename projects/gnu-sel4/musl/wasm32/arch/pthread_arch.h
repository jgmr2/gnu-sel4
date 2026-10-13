/* No thread pointer register: a global (src/thread/wasm32/__set_thread_area.c); the thread
 * structure at it, TLS below, as on x86 */
extern hidden uintptr_t __wasm_tp;
static inline uintptr_t __get_tp()
{
	return __wasm_tp;
}

#define MC_PC arm_pc
