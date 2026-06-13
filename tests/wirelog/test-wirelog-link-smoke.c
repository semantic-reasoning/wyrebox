#include <glib.h>

#if __has_include(<wyrelog/engine.h>)
#include <wyrelog/engine.h>
#define WYREBOX_WIRELOG_LINK_SMOKE_HAS_WYRELOG_ENGINE 1
#else
#include <wirelog/wirelog.h>
#include <wirelog/wirelog-types.h>
#endif

static void
test_wirelog_public_symbols_link (void)
{
#ifdef WYREBOX_WIRELOG_LINK_SMOKE_HAS_WYRELOG_ENGINE
  WylEngine *engine = NULL;
  wyrelog_error_t result = wyl_engine_open (NULL, 1, &engine);

  g_assert_cmpint (result, ==, WYRELOG_E_INVALID);
  g_assert_null (engine);
#else
  const char *eq_text = wirelog_cmp_op_str (WIRELOG_CMP_EQ);
  const char *add_text = wirelog_arith_op_str (WIRELOG_ARITH_ADD);

  g_assert_cmpstr (eq_text, ==, "=");
  g_assert_cmpstr (add_text, ==, "+");
#endif
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/link-smoke/public-symbols",
      test_wirelog_public_symbols_link);

  return g_test_run ();
}
