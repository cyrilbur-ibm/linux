/*
 * Copyright 2016, Cyril Bur, IBM Corp.
 * Licensed under GPLv2.
 *
 * Test the kernel's signal frame code.
 *
 * The kernel sets up two sets of ucontexts if the signal was to be delivered
 * while the thread was in a transaction. Expected behaviour is that the
 * currently executing code is in the first and the checkpointed state (the
 * state that will be rolled back to) is in the uc_link ucontext.
 *
 * The reason for this is that code which is not TM aware and installs a signal
 * handler will expect to see/modify its currently running state in the uc,
 * this code may have dynamicially linked against code which is TM aware and is
 * doing HTM under the hood.
 */

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include "utils.h"
#include "tm.h"

#define TBEGIN          ".long 0x7C00051D ;"
#define TSUSPEND        ".long 0x7C0005DD ;"
#define TRESUME         ".long 0x7C2005DD ;"
#define MAX_ATTEMPT 100

static double fps[] = { 1, 2, 3, 4, 5, 6, 7, 8,
						-1, -2, -3, -4, -5, -6, -7, -8 };

extern long tm_signal_self(pid_t pid, double *fps);

static int signaled;
static int fail;

static void signal_usr1(int signum, siginfo_t *info, void *uc)
{
	int i;
	ucontext_t *ucp = uc;
	ucontext_t *tm_ucp = ucp->uc_link;

	signaled = 1;

	/* Always be 64bit, don't really care about 32bit */
	for (i = 0; i < 8 && !fail; i++) {
		fail = (ucp->uc_mcontext.gp_regs[i + 14] != i);
		fail |= (tm_ucp->uc_mcontext.gp_regs[i + 14] != 0xFF - i);
	}
	if (fail) {
		printf("Failed on %d gpr %lu or %lu\n", i - 1, ucp->uc_mcontext.gp_regs[i + 13], tm_ucp->uc_mcontext.gp_regs[i + 13]);
		return;
	}
	for (i = 0; i < 8 && !fail; i++) {
		fail = (ucp->uc_mcontext.fp_regs[i + 14] != fps[i]);
		fail |= (tm_ucp->uc_mcontext.fp_regs[i + 14] != fps[i + 8]);
	}
	if (fail) {
		printf("Failed on %d FP %g or %g\n", i - 1, ucp->uc_mcontext.fp_regs[i + 13], tm_ucp->uc_mcontext.fp_regs[i + 13]);
	}
}

static int tm_signal_context_chk()
{
	struct sigaction act;
	int i;
	long rc;
	pid_t pid = getpid();

	SKIP_IF(!have_htm());

	act.sa_sigaction = signal_usr1;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_SIGINFO;
	if (sigaction(SIGUSR1, &act, NULL) < 0) {
		perror("sigaction sigusr1");
		exit(1);
	}

	i = 0;
	while (!signaled && i < MAX_ATTEMPT) {
		rc = tm_signal_self(pid, fps);
		if (!rc) {
			fprintf(stderr, "Transaction was not doomed...\n");
			FAIL_IF(!rc);
		}
		i++;
	}

	if (i == MAX_ATTEMPT) {
		fprintf(stderr, "Tried to signal %d times and didn't work, failing!\n", MAX_ATTEMPT);
		fail = 1;
	}
	return fail;
}

int main(void)
{
	return test_harness(tm_signal_context_chk, "tm_signal_context_chk");
}
