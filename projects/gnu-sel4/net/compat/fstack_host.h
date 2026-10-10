/* Included before everything else in netstack: what the F-Stack headers expect from glibc */
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <pthread.h>
typedef int64_t __int64_t;
typedef uint64_t __uint64_t;
