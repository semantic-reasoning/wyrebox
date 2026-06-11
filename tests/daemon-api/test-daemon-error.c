#include "wyrebox-daemon-error.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_error_class_round_trips_schema_names (void)
{
  WyreboxDaemonErrorClass error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR;

  g_assert_cmpstr (wyrebox_daemon_error_class_to_string
      (WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE), ==, "temporaryFailure");
  g_assert_cmpstr (wyrebox_daemon_error_class_to_string
      (WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE), ==, "permanentFailure");
  g_assert_cmpstr (wyrebox_daemon_error_class_to_string
      (WYREBOX_DAEMON_ERROR_PERMISSION_DENIED), ==, "permissionDenied");
  g_assert_cmpstr (wyrebox_daemon_error_class_to_string
      (WYREBOX_DAEMON_ERROR_NOT_FOUND), ==, "notFound");
  g_assert_cmpstr (wyrebox_daemon_error_class_to_string
      (WYREBOX_DAEMON_ERROR_CONFLICT), ==, "conflict");
  g_assert_cmpstr (wyrebox_daemon_error_class_to_string
      (WYREBOX_DAEMON_ERROR_INTERNAL_ERROR), ==, "internalError");

  g_assert_true (wyrebox_daemon_error_class_from_string ("temporaryFailure",
          &error_class));
  g_assert_cmpint (error_class, ==, WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE);
  g_assert_true (wyrebox_daemon_error_class_from_string ("permanentFailure",
          &error_class));
  g_assert_cmpint (error_class, ==, WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
  g_assert_true (wyrebox_daemon_error_class_from_string ("permissionDenied",
          &error_class));
  g_assert_cmpint (error_class, ==, WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
  g_assert_true (wyrebox_daemon_error_class_from_string ("notFound",
          &error_class));
  g_assert_cmpint (error_class, ==, WYREBOX_DAEMON_ERROR_NOT_FOUND);
  g_assert_true (wyrebox_daemon_error_class_from_string ("conflict",
          &error_class));
  g_assert_cmpint (error_class, ==, WYREBOX_DAEMON_ERROR_CONFLICT);
  g_assert_true (wyrebox_daemon_error_class_from_string ("internalError",
          &error_class));
  g_assert_cmpint (error_class, ==, WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

static void
test_error_class_rejects_unknown_schema_name (void)
{
  WyreboxDaemonErrorClass error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR;

  g_assert_false (wyrebox_daemon_error_class_from_string ("retryLater",
          &error_class));
  g_assert_cmpint (error_class, ==, WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
  g_assert_false (wyrebox_daemon_error_class_from_string (NULL, &error_class));
  g_assert_cmpint (error_class, ==, WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
  g_assert_null (wyrebox_daemon_error_class_to_string
      ((WyreboxDaemonErrorClass) 999));
}

static void
test_error_class_maps_g_io_errors_conservatively (void)
{
  g_assert_cmpint (wyrebox_daemon_error_class_from_g_error_code
      (G_IO_ERROR_PERMISSION_DENIED),
      ==, WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (wyrebox_daemon_error_class_from_g_error_code
      (G_IO_ERROR_NOT_FOUND), ==, WYREBOX_DAEMON_ERROR_NOT_FOUND);
  g_assert_cmpint (wyrebox_daemon_error_class_from_g_error_code
      (G_IO_ERROR_EXISTS), ==, WYREBOX_DAEMON_ERROR_CONFLICT);
  g_assert_cmpint (wyrebox_daemon_error_class_from_g_error_code
      (G_IO_ERROR_INVALID_ARGUMENT),
      ==, WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
  g_assert_cmpint (wyrebox_daemon_error_class_from_g_error_code
      (G_IO_ERROR_TIMED_OUT), ==, WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE);
  g_assert_cmpint (wyrebox_daemon_error_class_from_g_error_code
      (G_IO_ERROR_FAILED), ==, WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/error/class-round-trips-schema-names",
      test_error_class_round_trips_schema_names);
  g_test_add_func ("/daemon-api/error/class-rejects-unknown-schema-name",
      test_error_class_rejects_unknown_schema_name);
  g_test_add_func ("/daemon-api/error/class-maps-g-io-errors-conservatively",
      test_error_class_maps_g_io_errors_conservatively);

  return g_test_run ();
}
