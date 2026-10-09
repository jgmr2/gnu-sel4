/* Se incluye antes que todo en netstack: lo que las cabeceras de F-Stack esperan de glibc */
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <pthread.h>
typedef int64_t __int64_t;
typedef uint64_t __uint64_t;
