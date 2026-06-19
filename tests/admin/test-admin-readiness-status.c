#include "wyrebox-admin-readiness-status.h"

#include <glib.h>

static void
test_readiness_status_state_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_admin_readiness_status_state_to_name
      (WYREBOX_ADMIN_READINESS_STATUS_READY), ==, "ready");
  g_assert_cmpstr (wyrebox_admin_readiness_status_state_to_name
      (WYREBOX_ADMIN_READINESS_STATUS_REPLAYING), ==, "replaying");
  g_assert_cmpstr (wyrebox_admin_readiness_status_state_to_name
      (WYREBOX_ADMIN_READINESS_STATUS_MATERIALIZING), ==, "materializing");
  g_assert_cmpstr (wyrebox_admin_readiness_status_state_to_name
      (WYREBOX_ADMIN_READINESS_STATUS_READ_ONLY), ==, "read-only");
  g_assert_cmpstr (wyrebox_admin_readiness_status_state_to_name
      (WYREBOX_ADMIN_READINESS_STATUS_UNAVAILABLE), ==, "unavailable");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/admin/readiness-status/state-names-are-stable",
      test_readiness_status_state_names_are_stable);

  return g_test_run ();
}
