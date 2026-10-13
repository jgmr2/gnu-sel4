/* setjmp/longjmp as WebAssembly exceptions: what code built with clang's -mllvm
 * -wasm-enable-sjlj calls in place of setjmp and longjmp. Two interfaces: clang 18's (as
 * emscripten's: saveSetjmp/testSetjmp, a table of the function's setjmp calls, its size through
 * getTempRet0) and clang 19's on (as wasi-libc's: __wasm_setjmp/__wasm_setjmp_test). Both throw
 * with __wasm_longjmp; the first word of a jmp_buf is its setjmp's id */
#include <stdint.h>
#include <setjmp.h>
#include <stdlib.h>

struct jmp_impl {
	void *func_invocation_id;
	uint32_t label;
	struct { void *env; int val; } arg;   /* what the exception carries */
};

static uint32_t temp_ret0, setjmp_id;
uint32_t getTempRet0(void) { return temp_ret0; }
void setTempRet0(uint32_t v) { temp_ret0 = v; }

uint32_t *saveSetjmp(uint32_t *env, uint32_t label, uint32_t *table, uint32_t size)
{
	*env = ++setjmp_id;
	for (uint32_t i = 0; i < size; i++)
		if (!table[i * 2]) {
			table[i * 2] = setjmp_id;
			table[i * 2 + 1] = label;
			table[i * 2 + 2] = 0;         /* the end of the table */
			setTempRet0(size);
			return table;
		}
	size *= 2;                                /* full: twice as big */
	table = realloc(table, sizeof(uint32_t) * 2 * (size + 1));
	--setjmp_id;
	table = saveSetjmp(env, label, table, size);
	setTempRet0(size);
	return table;
}

uint32_t testSetjmp(uint32_t id, uint32_t *table, uint32_t size)
{
	for (uint32_t i = 0; i < size && table[i * 2]; i++)
		if (table[i * 2] == id) return table[i * 2 + 1];
	return 0;
}

void __wasm_setjmp(void *env, uint32_t label, void *func_invocation_id)
{
	struct jmp_impl *j = env;
	j->func_invocation_id = func_invocation_id;
	j->label = label;
}

uint32_t __wasm_setjmp_test(void *env, void *func_invocation_id)
{
	struct jmp_impl *j = env;
	return j->func_invocation_id == func_invocation_id ? j->label : 0;
}

void __wasm_longjmp(void *env, int val)
{
	struct jmp_impl *j = env;
	j->arg.env = env;
	j->arg.val = val ? val : 1;
	__builtin_wasm_throw(1, &j->arg);
}

/* Code built without -wasm-enable-sjlj that calls them by name */
_Noreturn void longjmp(jmp_buf env, int val)
{
	__wasm_longjmp(env, val);
	__builtin_unreachable();
}
_Noreturn void _longjmp(jmp_buf env, int val)
{
	__wasm_longjmp(env, val);
	__builtin_unreachable();
}
