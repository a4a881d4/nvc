//
//  Copyright (C) 2011-2012  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#define _GNU_SOURCE

#include "util.h"
#include "ident.h"

#if !defined __CYGWIN__
// Get REG_EIP from ucontext.h
#include <sys/ucontext.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#if !defined __CYGWIN__
#include <execinfo.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/wait.h>
#if !defined __CYGWIN__
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#endif

#ifdef __linux
#include <sys/prctl.h>
#endif  // __linux

// The IP register is different depending on the CPU arch
// Try x86-64 first then regular x86: nothing else is supported
#if defined REG_RIP
#define ARCH_IP_REG REG_RIP
#elif defined REG_EIP
#define ARCH_IP_REG REG_EIP
#elif defined __APPLE__
#ifdef __LP64__
#define ARCH_IP_REG __rip
#else
#define ARCH_IP_REG __eip
#endif
#elif defined __ppc__ || defined __powerpc__
#define ARCH_IP_REG __nip
#elif defined __CYGWIN__
#define NO_STACK_TRACE
#else
#warning "Don't know the IP register name for your architecture!"
#define NO_STACK_TRACE
#endif

#define N_TRACE_DEPTH   16
#define ERROR_SZ        1024
#define PAGINATE_RIGHT  72

#define ANSI_RESET      0
#define ANSI_BOLD       1
#define ANSI_FG_BLACK   30
#define ANSI_FG_RED     31
#define ANSI_FG_GREEN   32
#define ANSI_FG_YELLOW  33
#define ANSI_FG_BLUE    34
#define ANSI_FG_MAGENTA 35
#define ANSI_FG_CYAN    36
#define ANSI_FG_WHITE   37

#define MAX_FMT_BUFS    8
#define MAX_PRINTF_BUFS 8

typedef void (*print_fn_t)(const char *fmt, ...);

static void def_error_fn(const char *msg, const loc_t *loc);

struct option {
   struct option *next;
   ident_t       key;
   int           value;
};

struct prbuf {
   const char *buf;
   char       *wptr;
   size_t      remain;
};

struct color_escape {
   const char *name;
   int         value;
};

static error_fn_t     error_fn = def_error_fn;
static fatal_fn_t     fatal_fn = NULL;
static bool           want_color = false;
static struct option *options = NULL;
static struct prbuf   printf_bufs[MAX_PRINTF_BUFS];
static int            next_printf_buf = 0;

static const struct color_escape escapes[] = {
   { "",        ANSI_RESET },
   { "bold",    ANSI_BOLD },
   { "black",   ANSI_FG_BLACK },
   { "red",     ANSI_FG_RED },
   { "green",   ANSI_FG_GREEN },
   { "yellow",  ANSI_FG_YELLOW },
   { "blue",    ANSI_FG_BLUE },
   { "magenta", ANSI_FG_MAGENTA },
   { "cyan",    ANSI_FG_CYAN },
   { "white",   ANSI_FG_WHITE },
};

static char *filter_color(const char *str)
{
   // Replace color strings like "$red$foo$$bar" with ANSI escaped
   // strings like "\033[31mfoo\033[0mbar"

   char *copy = xmalloc(strlen(str) * 2);
   char *p = copy;

   const char *escape_start = NULL;

   while (*str != '\0') {
      if (*str == '$') {
         if (escape_start == NULL)
            escape_start = str;
         else {
            const char *e = escape_start + 1;
            const size_t len = str - e;

            if (want_color) {
               bool found = false;
               for (int i = 0; i < ARRAY_LEN(escapes); i++) {
                  if (strncmp(e, escapes[i].name, len) == 0) {
                     p += sprintf(p, "\033[%dm", escapes[i].value);
                     found = true;
                     break;
                  }
               }

               if (!found) {
                  strncpy(p, escape_start, len + 2);
                  p += len + 2;
               }
            }

            escape_start = NULL;
         }
      }
      else if (escape_start == NULL)
         *p++ = *str;

      ++str;
   }

   if (escape_start != NULL) {
      const size_t len = str - escape_start;
      strncpy(p, escape_start, len + 1);
      p += len + 1;
   }

   *p = '\0';

   return copy;
}

static void paginate_msg(const char *fmt, va_list ap, int left, int right)
{
   char *strp = NULL;
   if (vasprintf(&strp, fmt, ap) < 0)
      abort();

   char *filtered = filter_color(strp);

   const char *p = filtered;
   int col = left;
   while (*p != '\0') {
      if ((*p == '\n') || (isspace((uint8_t)*p) && col >= right)) {
         // Can break line here
         fputc('\n', stderr);
         for (col = 0; col < left; col++)
            fputc(' ', stderr);
      }
      else {
         fputc(*p, stderr);
         ++col;
      }
      ++p;
   }
   fputc('\n', stderr);

   free(filtered);
   free(strp);
}

static void set_attr(int attr)
{
   if (want_color)
      fprintf(stderr, "\033[%dm", attr);
}

void *xmalloc(size_t size)
{
   void *p = malloc(size);
   if (p == NULL)
      abort();
   return p;
}

void *xrealloc(void *ptr, size_t size)
{
   ptr = realloc(ptr, size);
   if (ptr == NULL)
      abort();
   return ptr;
}

static void fmt_color(int color, const char *prefix,
                      const char *fmt, va_list ap)
{
   set_attr(color);
   fprintf(stderr, "** %s: ", prefix);
   set_attr(ANSI_RESET);
   paginate_msg(fmt, ap, 10, PAGINATE_RIGHT);
}

void errorf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Error", fmt, ap);
   va_end(ap);
}

void warnf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_YELLOW, "Warning", fmt, ap);
   va_end(ap);
}

void notef(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_RESET, "Note", fmt, ap);
   va_end(ap);
}

static void def_error_fn(const char *msg, const loc_t *loc)
{
   errorf("%s", msg);
   fmt_loc(stderr, loc);
}

static char *prepare_msg(const char *fmt, va_list ap)
{
   char *color_fmt = filter_color(fmt);
   char *strp = NULL;
   if (vasprintf(&strp, color_fmt, ap) < 0)
      abort();
   free(color_fmt);
   return strp;
}

static void msg_at(print_fn_t fn, const loc_t *loc, const char *fmt, va_list ap)
{
   char *strp = prepare_msg(fmt, ap);
   (*fn)("%s", strp);
   fmt_loc(stderr, loc);
   free(strp);
}

void error_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   char *strp = prepare_msg(fmt, ap);
   error_fn(strp, loc != NULL ? loc : &LOC_INVALID);
   free(strp);

   va_end(ap);
}

void warn_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   msg_at(warnf, loc, fmt, ap);
   va_end(ap);
}

void note_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   msg_at(notef, loc, fmt, ap);
   va_end(ap);
}

void fatal_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
   fmt_loc(stderr, loc);
   va_end(ap);

   if (fatal_fn != NULL)
      (*fatal_fn)();

   exit(EXIT_FAILURE);
}

error_fn_t set_error_fn(error_fn_t fn)
{
   error_fn_t old = error_fn;
   error_fn = fn;
   return old;
}

void set_fatal_fn(fatal_fn_t fn)
{
   fatal_fn = fn;
}

void fatal(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
   va_end(ap);

   if (fatal_fn != NULL)
      (*fatal_fn)();

   exit(EXIT_FAILURE);
}

void fatal_trace(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
   va_end(ap);

#ifndef NO_STACK_TRACE
   show_stacktrace();
#endif  // !NO_STACK_TRACE

   exit(EXIT_FAILURE);
}

void fatal_errno(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   set_attr(ANSI_FG_RED);
   fprintf(stderr, "** Fatal: ");
   set_attr(ANSI_RESET);
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, ": %s\n", strerror(errno));

   va_end(ap);

   exit(EXIT_FAILURE);
}

void fmt_loc(FILE *f, const struct loc *loc)
{
   if ((loc == NULL) || (loc->first_line == LINE_INVALID))
      return;

   fprintf(f, "\tFile %s, Line %u\n", loc->file, loc->first_line);

   if (loc->linebuf == NULL)
      return;

   const char *lb = loc->linebuf;
   char buf[80];
   size_t i = 0;
   while (i < sizeof(buf) - 1 && *lb != '\0' && *lb != '\n') {
      if (*lb == '\t')
         buf[i++] = ' ';
      else
         buf[i++] = *lb;
      ++lb;
   }
   buf[i] = '\0';

   // Print ... if error location spans multiple lines
   bool many_lines = (loc->first_line != loc->last_line)
      || (i == sizeof(buf) - 1 && i <= loc->last_column);
   int last_col = many_lines ? strlen(buf) + 3 : loc->last_column;

   set_attr(ANSI_FG_CYAN);
   fprintf(f, "    %s%s\n", buf, many_lines ? " ..." : "");
   for (uint16_t j = 0; j < loc->first_column + 4; j++)
      fprintf(f, " ");
   set_attr(ANSI_FG_GREEN);
   for (uint16_t j = 0; j < last_col - loc->first_column + 1; j++)
      fprintf(f, "^");
   set_attr(ANSI_RESET);
   fprintf(f, "\n");
}

#ifndef NO_STACK_TRACE

static void print_trace(char **messages, int trace_size)
{
   int i;

   fputs("\n-------- STACK TRACE --------\n", stderr);
   for (i = 0; i < trace_size; i++) {
      fprintf(stderr, "%s\n", messages[i]);
   }
   fputs("-----------------------------\n", stderr);
}

void show_stacktrace(void)
{
   void *trace[N_TRACE_DEPTH];
   char **messages = NULL;
   int trace_size = 0;

   trace_size = backtrace(trace, N_TRACE_DEPTH);
   messages = backtrace_symbols(trace, trace_size);

   print_trace(messages, trace_size);

   free(messages);
}

static const char *signame(int sig)
{
   switch (sig) {
   case SIGSEGV: return "SIGSEGV";
   case SIGABRT: return "SIGABRT";
   case SIGILL: return "SIGILL";
   case SIGFPE: return "SIGFPE";
   case SIGUSR1: return "SIGUSR1";
   case SIGBUS: return "SIGBUS";
   default: return "???";
   }
}

static void bt_sighandler(int sig, siginfo_t *info, void *secret)
{
   void *trace[N_TRACE_DEPTH];
   char **messages = NULL;
   int trace_size = 0;
   ucontext_t *uc = (ucontext_t*)secret;

#ifdef __APPLE__
   uintptr_t ip = uc->uc_mcontext->__ss.ARCH_IP_REG;
#elif defined __ppc__ || defined __powerpc__
   uintptr_t ip = uc->uc_mcontext.regs->nip;
#else
   uintptr_t ip = uc->uc_mcontext.gregs[ARCH_IP_REG];
#endif

   fprintf(stderr, "\n*** Caught signal %d (%s)", sig, signame(sig));

   switch (sig) {
   case SIGSEGV:
   case SIGILL:
   case SIGFPE:
   case SIGBUS:
      fprintf(stderr, " [address=%p, ip=%p]", info->si_addr, (void*)ip);
      break;
   }

   fputs(" ***\n", stderr);

   trace_size = backtrace(trace, N_TRACE_DEPTH);

   // Overwrite sigaction with caller's address
   trace[1] = (void*)ip;

   messages = backtrace_symbols(trace, trace_size);

   // Skip first stack frame (points here)
   print_trace(messages + 1, trace_size - 1);

   free(messages);

   if (sig != SIGUSR1)
      exit(EXIT_FAILURE);
}

#endif  // NO_STACK_TRACE

static bool is_debugger_running(void)
{
#if defined __APPLE__

   struct kinfo_proc info;
   info.kp_proc.p_flag = 0;

   int mib[4];
   mib[0] = CTL_KERN;
   mib[1] = KERN_PROC;
   mib[2] = KERN_PROC_PID;
   mib[3] = getpid();

   size_t size = sizeof(info);
   int rc = sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);
   if (rc != 0)
      fatal_errno("sysctl");

   return (info.kp_proc.p_flag & P_TRACED) != 0;

#elif defined __linux

   // Hack to detect if Valgrind is running
   FILE *f = fopen("/proc/self/maps", "r");
   if (f != NULL) {
      char buf[1024];
      bool valgrind = false;
      while (!valgrind && fgets(buf, sizeof(buf), f)) {
         if (strstr(buf, "vgpreload"))
            valgrind = true;
      }
      fclose(f);
      if (valgrind)
         return true;
   }

#ifdef PR_SET_PTRACER
   // For Linux 3.4 and later allow tracing from any proccess

   if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) < 0)
      perror("prctl");

#endif  // PR_SET_PTRACER

   pid_t pid = fork();

   if (pid == -1)
      fatal_errno("fork");
   else if (pid == 0) {
      int ppid = getppid();

      // Try to trace the parent: if we can then GDB is not running
      if (ptrace(PTRACE_ATTACH, ppid, NULL, NULL) == 0) {
         // Wait for the parent to stop and continue it
         waitpid(ppid, NULL, 0);
         ptrace(PTRACE_CONT, NULL, NULL);

         // Detach
         ptrace(PTRACE_DETACH, ppid, NULL, NULL);

         // Able to trace so debugger not present
         exit(0);
      }
      else {
         // Trace failed so debugger is present
         exit(1);
      }
   }
   else {
      int status;
      waitpid(pid, &status, 0);
      return WEXITSTATUS(status);
   }

#else

   // Not able to detect debugger on this platform
   return false;

#endif
}

#ifdef __linux
static void gdb_sighandler(int sig, siginfo_t *info)
{
   char exe[256];
   if (readlink("/proc/self/exe", exe, sizeof(exe)) < 0)
      fatal_errno("readlink");

   char pid[16];
   snprintf(pid, sizeof(pid), "%d", getpid());

   pid_t p = fork();
   if (p == 0) {
      execl("/usr/bin/gdb", "gdb", "-ex", "cont", exe, pid, NULL);
      fatal_errno("execl");
   }
   else if (p < 0)
      fatal_errno("fork");
   else {
      // Allow a little time for GDB to start before dropping
      // into the default signal handler
      sleep(1);
      signal(sig, SIG_DFL);
   }
}
#endif  // __linux

void register_trace_signal_handlers(void)
{
#ifndef NO_STACK_TRACE
   if (is_debugger_running())
      return;

   struct sigaction sa;
   sa.sa_sigaction = (void*)bt_sighandler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART | SA_SIGINFO;

   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGUSR1, &sa, NULL);
   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);
#endif  // NO_STACK_TRACE
}

void register_gdb_signal_handlers(void)
{
#ifdef __linux
   if (is_debugger_running())
      return;

   struct sigaction sa;
   sa.sa_sigaction = (void*)gdb_sighandler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART | SA_SIGINFO;

   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGUSR1, &sa, NULL);
   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);
#else  // __linux
   register_trace_signal_handlers();
#endif  // __linux
}

void term_init(void)
{
   const char *nvc_no_color = getenv("NVC_NO_COLOR");
   const char *term = getenv("TERM");

   static const char *term_blacklist[] = {
      "dumb"
   };

   want_color = isatty(STDERR_FILENO) && (nvc_no_color == NULL);

   if (want_color && (term != NULL)) {
      for (size_t i = 0; i < ARRAY_LEN(term_blacklist); i++) {
         if (strcmp(term, term_blacklist[i]) == 0) {
            want_color = false;
            break;
         }
      }
   }
}

void opt_set_int(const char *name, int val)
{
   ident_t name_i = ident_new(name);
   struct option *it;
   for (it = options; (it != NULL) && (it->key != name_i); it = it->next)
      ;

   if (it != NULL)
      it->value = val;
   else {
      it = xmalloc(sizeof(struct option));
      it->key   = ident_new(name);
      it->value = val;
      it->next  = options;

      options = it;
   }
}

int opt_get_int(const char *name)
{
   ident_t name_i = ident_new(name);
   struct option *it;
   for (it = options; (it != NULL) && (it->key != name_i); it = it->next)
      ;

   if (it != NULL)
      return it->value;
   else
      fatal("invalid option %s", name);
}

char *get_fmt_buf(size_t len)
{
   // This is a bit of a kludge but keeping a sufficient number
   // of static buffers allows us to use format functions multiple
   // times in printf
   static char   *buf_set[MAX_FMT_BUFS];
   static size_t  buflen[MAX_FMT_BUFS];
   static int     next_buf = 0;

   char **bufp = &buf_set[next_buf];
   size_t *blenp = &buflen[next_buf];
   next_buf = (next_buf + 1) % MAX_FMT_BUFS;

   if (*bufp == NULL) {
      *bufp = xmalloc(len);
      *blenp = len;
   }

   while (len > *blenp) {
      *blenp *= 2;
      *bufp = xrealloc(*bufp, *blenp);
   }

   return *bufp;
}

void static_printf_begin(char *buf, size_t len)
{
   next_printf_buf = (next_printf_buf + 1) % MAX_PRINTF_BUFS;

   struct prbuf *p = &(printf_bufs[next_printf_buf]);
   p->buf    = buf;
   p->wptr   = buf;
   p->remain = len;

   buf[len - 1] = '\0';
}

void static_printf(char *buf, const char *fmt, ...)
{
   struct prbuf *p = NULL;
   int i = next_printf_buf;
   do {
      if (printf_bufs[i].buf == buf)
         p = &(printf_bufs[i]);
      i = (i == 0) ? (MAX_PRINTF_BUFS - 1) : (i - 1);
   } while ((p == NULL) && (i != next_printf_buf));
   assert(p != NULL);

   if (p->remain == 0)
      return;

   va_list ap;
   va_start(ap, fmt);

   int n = vsnprintf(p->wptr, p->remain, fmt, ap);
   if ((n < 0) || (n > p->remain))
      p->remain = 0;
   else {
      p->remain -= n;
      p->wptr   += n;
   }

   va_end(ap);
}

int next_power_of_2(int n)
{
   n--;
   n |= n >> 1;
   n |= n >> 2;
   n |= n >> 4;
   n |= n >> 8;
   n |= n >> 16;
   n++;
   return n;
}
