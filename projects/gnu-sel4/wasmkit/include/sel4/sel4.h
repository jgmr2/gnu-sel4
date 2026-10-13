/*
 * wasmkit: the few seL4 types and calls Microkit components use, for the browser port (TODO
 * item 24). There is no seL4 kernel underneath yet: wasmkit.c does what Microkit's runtime and
 * the kernel do for them (notifications, protected procedure calls, message registers).
 * seL4_Word stays 64 bits on wasm32: the components' protocols carry 64-bit values in it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t seL4_Word;
typedef uint64_t seL4_CPtr;
typedef uint8_t seL4_Uint8;
typedef uint16_t seL4_Uint16;
typedef uint32_t seL4_Uint32;
typedef uint64_t seL4_Uint64;
typedef bool seL4_Bool;
typedef struct { seL4_Word label; seL4_Uint16 count; } seL4_MessageInfo_t;

#define seL4_MsgMaxLength 120

seL4_Word seL4_GetMR(int i);
void seL4_SetMR(int i, seL4_Word v);
