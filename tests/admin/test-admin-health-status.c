#include "wyrebox-admin-health-status.h"

#include <glib.h>

static void
test_health_status_state_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_HEALTHY), ==, "healthy");
  g_assert_cmpstr (wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_DEGRADED), ==, "degraded");
  g_assert_cmpstr (wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_REPLAYING), ==, "replaying");
  g_assert_cmpstr (wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_MATERIALIZING), ==, "materializing");
  g_assert_cmpstr (wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_READ_ONLY), ==, "read-only");
  g_assert_cmpstr (wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_STORAGE_FULL), ==, "storage-full");
  g_assert_cmpstr (wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_STORAGE_ERROR), ==, "storage-error");
  g_assert_cmpstr (wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_UNAVAILABLE), ==, "unavailable");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/admin/health-status/state-names-are-stable",
      test_health_status_state_names_are_stable);

  return g_test_run ();
}
