/*
 * Copyright 2015, Michael Ellerman, IBM Corp.
 * Licensed under GPLv2.
 */

#ifndef _SELFTESTS_POWERPC_TM_TM_H
#define _SELFTESTS_POWERPC_TM_TM_H

#include <stdbool.h>
#include <asm/cputable.h>

#include "../utils.h"

static inline bool have_htm(void)
{
#ifdef PPC_FEATURE2_HTM
	return have_hwcap2(PPC_FEATURE2_HTM);
#else
	printf("PPC_FEATURE2_HTM not defined, can't check AT_HWCAP2\n");
	return false;
#endif
}

static inline bool have_htm_nosc(void)
{
#ifdef PPC_FEATURE2_HTM_NOSC
	return have_hwcap2(PPC_FEATURE2_HTM_NOSC);
#else
	printf("PPC_FEATURE2_HTM_NOSC not defined, can't check AT_HWCAP2\n");
	return false;
#endif
}

long tm_signal_self_context_load(pid_t pid,
		long *gps, double *fps, vector int *vms,
		vector int *vss);

#endif /* _SELFTESTS_POWERPC_TM_TM_H */
