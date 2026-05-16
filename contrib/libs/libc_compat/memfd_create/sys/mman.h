#pragma once

#include_next <sys/mman.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MFD_CLOEXEC
#   define MFD_CLOEXEC 		0x0001U
#endif
#define MADV_WIPEONFORK  	18

int memfd_create(const char *name, unsigned flags);

#ifdef __cplusplus
} // extern "C"
#endif
