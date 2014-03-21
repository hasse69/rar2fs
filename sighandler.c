/*
    Copyright (C) 2009-2014 Hans Beckerus (hans.beckerus#AT#gmail.com)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    This program take use of the freeware "Unrar C++ Library" (libunrar)
    by Alexander Roshal and some extensions to it.

    Unrar source may be used in any software to handle RAR archives
    without limitations free of charge, but cannot be used to re-create
    THE rAR compression algorithm, which is proprietary. Distribution
    of modified Unrar source in separate form or as a part of other
    software is permitted, provided that it is clearly stated in
    the documentation and source comments that the code may not be used
    to develop a RAR (WinRAR) compatible archiver.
*/

#include <platform.h>
#include <signal.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/wait.h>
#include "debug.h"
#include "filecache.h"

#ifdef HAVE_STRUCT_SIGACTION_SA_SIGACTION
#if !defined ( HAVE_EXECINFO_H ) || !defined ( HAVE_UCONTEXT_H )
#define stack_trace(a,b,c) (void)(a);(void)(b);(void)(c)
#else
#include <execinfo.h>
#include <ucontext.h>

/*!
 *****************************************************************************
 *
 ****************************************************************************/

#ifndef REG_PC
#ifdef REG_EIP
#define REG_PC REG_EIP
#endif
#endif

static void stack_trace(int sig, siginfo_t *info, void *secret)
{
#ifdef REG_PC
        ucontext_t *uc = (ucontext_t *)secret;
#else
        /* not used */
        (void)secret;  
#endif

        /* Do something useful with siginfo_t */
        char buf[256];
        snprintf(buf, sizeof(buf), "Got signal %d, faulty address is %p, "
                        "from %p", sig, info->si_addr,
#ifdef REG_PC
        (void*)uc->uc_mcontext.gregs[REG_PC]);
#else
        /* TODO: need to handle compilers other than GCC */
        __builtin_return_address(0));
#endif
        printf("%s\n", buf);
        syslog(LOG_INFO, "%s", buf);

        void *trace[30];
        char **messages = (char **)NULL;
        int i;
        int trace_size = 0;

        trace_size = backtrace(trace, 30);
        /* overwrite sigaction with caller's address */
#ifdef REG_PC
        trace[1] = (void *) uc->uc_mcontext.gregs[REG_PC];
#else
        /* TODO: need to handle compilers other than GCC */
        trace[1] = __builtin_return_address(0);
#endif
        messages = backtrace_symbols(trace, trace_size);
        if (messages) {
                /* skip first stack frame (points here) */
                for (i = 1; i < trace_size; ++i) {
                        printf("%s\n", messages[i]);
                        syslog(LOG_INFO, "%s", messages[i]);
                }
                free(messages);
        }
}
#endif
#endif


/*!
 *****************************************************************************
 *
 ****************************************************************************/

int glibc_test = 0;

#ifdef HAVE_STRUCT_SIGACTION_SA_SIGACTION
static RETSIGTYPE sig_handler(int signum, siginfo_t *info, void* secret)
#else
static RETSIGTYPE sig_handler(int signum)
#endif
{
        switch(signum)
        {
        case SIGUSR1:
                printd(4, "Caught signal SIGUSR1\n");
                printd(3, "Invalidating path cache\n");
                pthread_mutex_lock(&file_access_mutex);
                filecache_invalidate(NULL);
                pthread_mutex_unlock(&file_access_mutex);
                break;
        case SIGSEGV:
                if (!glibc_test) {
                        printd(4, "Caught signal SIGSEGV\n");
#ifdef HAVE_STRUCT_SIGACTION_SA_SIGACTION
                        stack_trace(SIGSEGV, info, secret);
#endif
                } else {
                        printf("glibc validation failed\n");
                }
                exit(EXIT_FAILURE);
        case SIGCHLD:
                printd(4, "Caught signal SIGCHLD\n");
                break;
        }
#if RETSIGTYPE != void
        return (RETSIGTYPE)0;
#endif
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void sighandler_init()
{
        struct sigaction act;

#if 0
        /* Avoid child zombies for SIGCHLD */
        sigaction(SIGCHLD, NULL, &act);
#ifdef HAVE_STRUCT_SIGACTION_SA_SIGACTION
        act.sa_sigaction = sig_handler;
        act.sa_flags |= SA_SIGINFO;
#else
        act.sa_handler = sig_handler;
#endif
        act.sa_flags |= (SA_NOCLDWAIT);
        sigaction(SIGCHLD, &act, NULL);
#endif

        sigaction(SIGUSR1, NULL, &act);
        sigemptyset(&act.sa_mask);
#ifdef HAVE_STRUCT_SIGACTION_SA_SIGACTION
        act.sa_sigaction = sig_handler;
        act.sa_flags = SA_SIGINFO;
#else
        act.sa_handler = sig_handler;
        act.sa_flags = 0;
#endif
        /* make sure a system call is restarted to avoid exit */
        act.sa_flags |= SA_RESTART;
        sigaction(SIGUSR1, &act, NULL);

        sigaction(SIGSEGV, NULL, &act);
        sigemptyset(&act.sa_mask);
#ifdef HAVE_STRUCT_SIGACTION_SA_SIGACTION
        act.sa_sigaction = sig_handler;
        act.sa_flags = SA_SIGINFO;
#else
        act.sa_handler = sig_handler;
        act.sa_flags = 0;
#endif
        sigaction(SIGSEGV, &act, NULL);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void sighandler_destroy()
{
}

