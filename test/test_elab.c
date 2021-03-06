#include "parse.h"
#include "type.h"
#include "util.h"
#include "phase.h"

#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

typedef struct error {
   int        line;
   const char *snippet;
} error_t;

static const error_t  *error_lines = NULL;
static error_fn_t orig_error_fn = NULL;

void cover_tag(void)
{
   assert(false);
}

static void setup(void)
{
   lib_set_work(lib_tmp());
   opt_set_int("bootstrap", 0);
   opt_set_int("cover", 0);
}

static void teardown(void)
{
   lib_free(lib_work());
}

static void test_error_fn(const char *msg, const loc_t *loc)
{
   fail_if(error_lines == NULL);

   bool unexpected = error_lines->line == -1
      || error_lines->snippet == NULL
      || error_lines->line != loc->first_line
      || strstr(msg, error_lines->snippet) == NULL;

   if (unexpected) {
      orig_error_fn(msg, loc);
      printf("expected line %d '%s'\n",
             error_lines->line, error_lines->snippet);
   }

   fail_if(unexpected);

   error_lines++;
}

static void expect_errors(const error_t *lines)
{
   fail_unless(orig_error_fn == NULL);
   orig_error_fn = set_error_fn(test_error_fn);
   error_lines = lines;
}

static tree_t run_elab(void)
{
   tree_t t, last_ent = NULL;
   while ((t = parse())) {
      sem_check(t);
      fail_if(sem_errors() > 0);

      simplify(t);

      if (tree_kind(t) == T_ENTITY)
         last_ent = t;
   }

   return elab(last_ent);
}

START_TEST(test_elab1)
{
   tree_t top;

   input_from_file(TESTDIR "/elab/elab1.vhd");

   const error_t expect[] = {
      { -1, NULL }
   };
   expect_errors(expect);

   top = run_elab();
}
END_TEST

START_TEST(test_elab2)
{
   tree_t top;

   input_from_file(TESTDIR "/elab/elab2.vhd");

   const error_t expect[] = {
      { -1, NULL }
   };
   expect_errors(expect);

   top = run_elab();
}
END_TEST

START_TEST(test_elab3)
{
   tree_t top;

   input_from_file(TESTDIR "/elab/elab3.vhd");

   const error_t expect[] = {
      { -1, NULL }
   };
   expect_errors(expect);

   top = run_elab();
}
END_TEST

START_TEST(test_elab4)
{
   tree_t top;

   input_from_file(TESTDIR "/elab/elab4.vhd");

   const error_t expect[] = {
      { 21, "actual width 9 does not match formal width 8" },
      { -1, NULL }
   };
   expect_errors(expect);

   top = run_elab();
}
END_TEST

START_TEST(test_open)
{
   tree_t top;

   input_from_file(TESTDIR "/elab/open.vhd");

   const error_t expect[] = {
      { -1, NULL }
   };
   expect_errors(expect);

   top = run_elab();
   opt(top);

   // Optimisation should have deleted all statements
   fail_unless(tree_stmts(top) == 0);
}
END_TEST

START_TEST(test_genagg)
{
   input_from_file(TESTDIR "/elab/genagg.vhd");

   const error_t expect[] = {
      { -1, NULL }
   };
   expect_errors(expect);

   (void)run_elab();
}
END_TEST

int main(void)
{
   register_trace_signal_handlers();

   setenv("NVC_LIBPATH", "../lib/std", 1);

   Suite *s = suite_create("elab");

   TCase *tc_core = tcase_create("Core");
   tcase_add_unchecked_fixture(tc_core, setup, teardown);
   tcase_add_test(tc_core, test_elab1);
   tcase_add_test(tc_core, test_elab2);
   tcase_add_test(tc_core, test_elab3);
   tcase_add_test(tc_core, test_elab4);
   tcase_add_test(tc_core, test_open);
   tcase_add_test(tc_core, test_genagg);
   suite_add_tcase(s, tc_core);

   SRunner *sr = srunner_create(s);
   srunner_run_all(sr, CK_NORMAL);

   int nfail = srunner_ntests_failed(sr);

   srunner_free(sr);

   return nfail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
