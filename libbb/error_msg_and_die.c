/* vi: set sw=4 ts=4: */
/*
 * Utility routines.
 *
 * Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 */

#include "libbb.h"

int die_sleep;
#if ENABLE_FEATURE_PREFER_APPLETS
jmp_buf die_jmp;
#endif

void xfunc_die(void)
{
	if (die_sleep) {
		if (ENABLE_FEATURE_PREFER_APPLETS && die_sleep < 0) {
			/* Special case. We arrive here if NOFORK applet
			 * calls xfunc, which then decides to die.
			 * We don't die, but jump instead back to caller.
			 * NOFORK applets still cannot carelessly call xfuncs:
			 * p = xmalloc(10);
			 * q = xmalloc(10); // BUG! if this dies, we leak p!
			 */
			/* -111 means "zero" (longjmp can't pass 0)
			 * spawn_and_wait() catches -111. */
			longjmp(die_jmp, xfunc_error_retval ? xfunc_error_retval : -111);
		}
		sleep(die_sleep);
	}
	exit(xfunc_error_retval);
}

void bb_error_msg_and_die(const char *s, ...)
{
	va_list p;

	va_start(p, s);
	bb_verror_msg(s, p, NULL);
	va_end(p);
	xfunc_die();
}
