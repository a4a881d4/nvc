//
//  Copyright (C) 2011-2013  Nick Gasson
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

#include "util.h"
#include "parse.h"
#include "phase.h"
#include "rt/rt.h"
#include "rt/slave.h"

#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#if defined HAVE_TCL_TCL_H
#include <tcl/tcl.h>
#elif defined HAVE_TCL_H
#include <tcl.h>
#endif

static const char *work_name = "work";

const char *copy_string =
   "Copyright (C) 2011-2013  Nick Gasson\n"
   "This program comes with ABSOLUTELY NO WARRANTY. This is free software, "
   "and\nyou are welcome to redistribute it under certain conditions. See "
   "the GNU\nGeneral Public Licence for details.";
const char *version_string =
   PACKAGE_STRING " (llvm " LLVM_VERSION "; tcl " TCL_VERSION ")";

static void set_work_lib(void)
{
   lib_t work = lib_find(work_name, false, false);
   if (work == NULL) {
      if ((work = lib_new(work_name)) == NULL)
         exit(EXIT_FAILURE);
   }

   lib_set_work(work);
}

static ident_t to_unit_name(const char *str)
{
   char *name = strdup(str);
   for (char *p = name; *p; p++)
      *p = toupper((uint8_t)*p);

   ident_t i = ident_prefix(lib_name(lib_work()),
                            ident_new(name), '.');
   free(name);
   return i;
}

static int analyse(int argc, char **argv)
{
   set_work_lib();

   static struct option long_options[] = {
      {"bootstrap", no_argument, 0, 'b'},
      {"dump-llvm", no_argument, 0, 'd'},
      {0, 0, 0, 0}
   };

   int c, index = 0;
   const char *spec = "";
   optind = 1;
   while ((c = getopt_long(argc, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         // getopt_long already printed an error message
         exit(EXIT_FAILURE);
      case 'b':
         opt_set_int("bootstrap", 1);
         break;
      case 'd':
         opt_set_int("dump-llvm", 1);
         break;
      default:
         abort();
      }
   }

   size_t unit_list_sz = 32;
   tree_t *units = xmalloc(sizeof(tree_t) * unit_list_sz);
   int n_units = 0;

   for (int i = optind; i < argc; i++) {
      input_from_file(argv[i]);

      tree_t unit;
      while ((unit = parse()) && sem_check(unit)) {
         if (n_units == unit_list_sz) {
            unit_list_sz *= 2;
            units = xrealloc(units, sizeof(tree_t) * unit_list_sz);
         }
         units[n_units++] = unit;
      }
   }

   for (int i = 0; i < n_units; i++) {
      unalias(units[i]);
      simplify(units[i]);
   }

   if (parse_errors() + sem_errors() + simplify_errors() > 0)
      return EXIT_FAILURE;

   lib_save(lib_work());

   for (int i = 0; i < n_units; i++) {
      tree_kind_t kind = tree_kind(units[i]);
      const bool need_cgen =
         (kind == T_PACK_BODY)
         || ((kind == T_PACKAGE) && pack_needs_cgen(units[i]));

      if (need_cgen)
         cgen(units[i]);
   }

   free(units);

   return EXIT_SUCCESS;
}

static int elaborate(int argc, char **argv)
{
   set_work_lib();

   static struct option long_options[] = {
      {"disable-opt", no_argument, 0, 'o'},
      {"dump-llvm", no_argument, 0, 'd'},
      {"native", no_argument, 0, 'n'},
      {"cover", no_argument, 0, 'c'},
      {0, 0, 0, 0}
   };

   int c, index = 0;
   const char *spec = "";
   optind = 1;
   while ((c = getopt_long(argc, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 'o':
         opt_set_int("optimise", 0);
         break;
      case 'd':
         opt_set_int("dump-llvm", 1);
         break;
      case 'n':
         opt_set_int("native", 1);
         break;
      case 'c':
         opt_set_int("cover", 1);
         break;
      case 0:
         // Set a flag
         break;
      case '?':
         // getopt_long already printed an error message
         exit(EXIT_FAILURE);
      default:
         abort();
      }
   }

   if (optind == argc)
      fatal("missing top-level unit name");

   ident_t unit_i = to_unit_name(argv[optind]);
   tree_t unit = lib_get(lib_work(), unit_i);
   if (unit == NULL)
      fatal("cannot find unit %s in library %s",
            istr(unit_i), istr(lib_name(lib_work())));

   tree_t e = elab(unit);
   if (e == NULL)
      return EXIT_FAILURE;

   opt(e);
   group_nets(e);

   // Save the library now so the code generator can attach temporary
   // meta data to trees
   lib_save(lib_work());

   cgen(e);
   link_bc(e);

   return EXIT_SUCCESS;
}

static uint64_t parse_time(const char *str)
{
   char     unit[4];
   unsigned base;
   uint64_t mult = 1;

   if (sscanf(str, "%u%3s", &base, unit) != 2)
      fatal("invalid time format: %s", str);

   if      (strcmp(unit, "fs") == 0)  mult = 1;
   else if (strcmp(unit, "ps") == 0)  mult = 1000;
   else if (strcmp(unit, "ns") == 0)  mult = 1000000;
   else if (strcmp(unit, "us") == 0)  mult = 1000000000;
   else if (strcmp(unit, "ms") == 0)  mult = 1000000000000;
   else if (strcmp(unit, "sec") == 0) mult = 1000000000000000;
   else
      fatal("invalid unit: %s", unit);

   return base * mult;
}

static int run(int argc, char **argv)
{
   set_work_lib();

   static struct option long_options[] = {
      { "trace",     no_argument,       0, 't' },
      { "batch",     no_argument,       0, 'b' },
      { "command",   no_argument,       0, 'c' },
      { "stop-time", required_argument, 0, 's' },
      { "vcd",       required_argument, 0, 'v' },
      { "stats",     no_argument,       0, 'S' },
      { "wave",      optional_argument, 0, 'w' },
      { 0, 0, 0, 0 }
   };

   enum { BATCH, COMMAND } mode = BATCH;

   uint64_t stop_time = UINT64_MAX;
   const char *vcd_fname = NULL;
   const char *lxt_fname = NULL;

   int c, index = 0;
   const char *spec = "bcw::";
   optind = 1;
   while ((c = getopt_long(argc, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         // getopt_long already printed an error message
         exit(EXIT_FAILURE);
      case 't':
         opt_set_int("rt_trace_en", 1);
         break;
      case 'b':
         mode = BATCH;
         break;
      case 'c':
         mode = COMMAND;
         break;
      case 's':
         stop_time = parse_time(optarg);
         break;
      case 'v':
         vcd_fname = optarg;
         break;
      case 'S':
         opt_set_int("rt-stats", 1);
         break;
      case 'w':
         if (optarg == NULL)
            lxt_fname = "";
         else
            lxt_fname = optarg;
         break;
      default:
         abort();
      }
   }

   if (optind == argc)
      fatal("missing top-level unit name");

   ident_t top = to_unit_name(argv[optind]);
   ident_t ename = ident_prefix(top, ident_new("elab"), '.');
   tree_rd_ctx_t ctx;
   tree_t e = lib_get_ctx(lib_work(), ename, &ctx);
   if (e == NULL)
      fatal("%s not elaborated", istr(top));
   else if (tree_kind(e) != T_ELAB)
      fatal("%s not suitable top level", istr(top));

   if (vcd_fname != NULL)
      vcd_init(vcd_fname, e);

   if (lxt_fname != NULL) {
      char tmp[128];
      if (*lxt_fname == '\0') {
         snprintf(tmp, sizeof(tmp), "%s.lxt", argv[optind]);
         lxt_fname = tmp;
         notef("writing LXT waveform data to %s", lxt_fname);
      }
      lxt_init(lxt_fname, e);
   }

   if (mode == BATCH)
      rt_batch_exec(e, stop_time, ctx);
   else {
      bool master = slave_fork();
      if (master)
         shell_run(e, ctx);
      else
         rt_slave_exec(e, ctx);
   }

   tree_read_end(ctx);
   return EXIT_SUCCESS;
}

static int dump_cmd(int argc, char **argv)
{
   set_work_lib();

   static struct option long_options[] = {
      {"elab", no_argument, 0, 'e'},
      {"body", no_argument, 0, 'b'},
      {"nets", no_argument, 0, 'n'},
      {0, 0, 0, 0}
   };

   bool add_elab = false, add_body = false, nets = false;
   int c, index = 0;
   const char *spec = "eb";
   optind = 1;
   while ((c = getopt_long(argc, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         // getopt_long already printed an error message
         exit(EXIT_FAILURE);
      case 'e':
         add_elab = true;
         break;
      case 'b':
         add_body = true;
         break;
      case 'n':
         add_elab = true;
         nets = true;
         break;
      default:
         abort();
      }
   }

   if (optind == argc)
      fatal("missing unit name");

   for (int i = optind; i < argc; i++) {
      ident_t name = to_unit_name(argv[i]);
      if (add_elab)
         name = ident_prefix(name, ident_new("elab"), '.');
      else if (add_body)
         name = ident_prefix(name, ident_new("body"), '-');
      tree_t top = lib_get(lib_work(), name);
      if (top == NULL)
         fatal("%s not analysed", istr(name));
      (nets ? dump_nets : dump)(top);
   }

   return EXIT_SUCCESS;
}

static void set_default_opts(void)
{
   opt_set_int("rt-stats", 0);
   opt_set_int("rt_trace_en", 0);
   opt_set_int("dump-llvm", 0);
   opt_set_int("optimise", 1);
   opt_set_int("native", 0);
   opt_set_int("bootstrap", 0);
   opt_set_int("cover", 0);
}

static void usage(void)
{
   printf("Usage: %s [OPTION]... COMMAND [OPTION]...\n"
          "\n"
          "COMMAND is one of:\n"
          " -a [OPTION]... FILE...\tAnalyse FILEs into work library\n"
          " -e UNIT\t\tElaborate and generate code for UNIT\n"
          " -r UNIT\t\tExecute previously elaborated UNIT\n"
          " --dump UNIT\t\tPrint out previously analysed UNIT\n"
          "\n"
          "Global options may be placed before COMMAND:\n"
          " -v, --version\t\tDisplay version and copyright information\n"
          " -h, --help\t\tDisplay this message and exit\n"
          "     --work=NAME\tUse NAME as the work library\n"
          "\n"
          "Analyse options:\n"
          "     --bootstrap\tAllow compilation of STANDARD package\n"
          "\n"
          "Elaborate options:\n"
          "     --cover\t\tEnable code coverage reporting\n"
          "     --disable-opt\tDisable LLVM optimisations\n"
          "     --dump-llvm\tPrint generated LLVM IR\n"
          "     --native\t\tGenerate native code shared library\n"
          "\n"
          "Run options:\n"
          " -b, --batch\t\tRun in batch mode (default)\n"
          " -c, --command\t\tRun in TCL command line mode\n"
          "     --stats\t\tPrint statistics at end of run\n"
          "     --stop-time=T\tStop after simulation time T (e.g. 5ns)\n"
          "     --trace\t\tTrace simulation events\n"
          "     --vcd=FILE\t\tWrite VCD data to FILE\n"
          " -w, --wave=FILE\tWrite waveform data in LXT format\n"
          "\n"
          "Dump options:\n"
          " -e, --elab\t\tDump an elaborated unit\n"
          " -b, --body\t\tDump package body\n"
          "     --nets\t\tShow mapping from signals to nets\n"
          "\n"
          "Report bugs to %s\n",
          PACKAGE, PACKAGE_BUGREPORT);
}

int main(int argc, char **argv)
{
   term_init();
   set_default_opts();

   if (getenv("NVC_GDB") != NULL)
      register_gdb_signal_handlers();
   else
      register_trace_signal_handlers();

   atexit(fbuf_cleanup);

   static struct option long_options[] = {
      {"help",    no_argument,       0, 'h'},
      {"version", no_argument,       0, 'v'},
      {"work",    required_argument, 0, 'w'},
      {"dump",    no_argument,       0, 'd'},
      {0, 0, 0, 0}
   };

   int c, index = 0;
   const char *spec = "aehr";
   while ((c = getopt_long(argc, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case 'h':
         usage();
         exit(EXIT_SUCCESS);
      case 'v':
         printf("%s\n%s\n", version_string, copy_string);
         exit(EXIT_SUCCESS);
      case 'w':
         work_name = optarg;
         break;
      case 'a':
      case 'e':
      case 'd':
      case 'r':
         // Subcommand options are parsed later
         argc -= (optind - 1);
         argv += (optind - 1);
         goto getopt_out;
      case '?':
         // getopt_long already printed an error message
         exit(EXIT_FAILURE);
      default:
         abort();
      }
   }
 getopt_out:

   switch (c) {
   case 'a':
      return analyse(argc, argv);
   case 'e':
      return elaborate(argc, argv);
   case 'r':
      return run(argc, argv);
   case 'd':
      return dump_cmd(argc, argv);
   default:
      fprintf(stderr, "%s: missing command\n", PACKAGE);
      return EXIT_FAILURE;
   }
}
