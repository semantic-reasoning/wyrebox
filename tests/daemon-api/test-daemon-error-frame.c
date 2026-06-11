#include "wyrebox-daemon-error-frame.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_error_frame_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonErrorFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_error_frame_init (&frame,
          "request-1",
          WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE,
          "try again later", "retry", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpint (frame.error_class, ==,
      WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE);
  g_assert_cmpstr (frame.message, ==, "try again later");
  g_assert_cmpstr (frame.retry_hint, ==, "retry");
}

static void
test_error_frame_allows_missing_retry_hint (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonErrorFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_error_frame_init (&frame,
          "request-2",
          WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
          "validation failed", "", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-2");
  g_assert_cmpint (frame.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
  g_assert_cmpstr (frame.message, ==, "validation failed");
  g_assert_null (frame.retry_hint);
}

static void
test_error_frame_rejects_missing_request_id (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonErrorFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_error_frame_init (&frame,
          "",
          WYREBOX_DAEMON_ERROR_INTERNAL_ERROR, "bad request", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (frame.request_id);
  g_assert_null (frame.message);
  g_assert_null (frame.retry_hint);
}

static void
test_error_frame_rejects_missing_message (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonErrorFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_error_frame_init (&frame,
          "request-3", WYREBOX_DAEMON_ERROR_INTERNAL_ERROR, "", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_error_frame_from_g_error_maps_class_and_message (void)
{
  g_autoptr (GError) cause = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonErrorFrame) frame = { 0 };

  g_set_error (&cause,
      G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "daemon request timed out");

  g_assert_true (wyrebox_daemon_error_frame_init_from_g_error (&frame,
          "request-4", cause, "retry later", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-4");
  g_assert_cmpint (frame.error_class, ==,
      WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE);
  g_assert_cmpstr (frame.message, ==, "daemon request timed out");
  g_assert_cmpstr (frame.retry_hint, ==, "retry later");
}

static void
test_error_frame_from_non_gio_error_is_internal (void)
{
  g_autoptr (GError) cause = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonErrorFrame) frame = { 0 };

  g_set_error_literal (&cause,
      G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE, "unexpected parser failure");

  g_assert_true (wyrebox_daemon_error_frame_init_from_g_error (&frame,
          "request-5", cause, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.error_class, ==, WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
  g_assert_cmpstr (frame.message, ==, "unexpected parser failure");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/error-frame/copies-fields",
      test_error_frame_copies_fields);
  g_test_add_func ("/daemon-api/error-frame/allows-missing-retry-hint",
      test_error_frame_allows_missing_retry_hint);
  g_test_add_func ("/daemon-api/error-frame/rejects-missing-request-id",
      test_error_frame_rejects_missing_request_id);
  g_test_add_func ("/daemon-api/error-frame/rejects-missing-message",
      test_error_frame_rejects_missing_message);
  g_test_add_func ("/daemon-api/error-frame/from-g-error",
      test_error_frame_from_g_error_maps_class_and_message);
  g_test_add_func ("/daemon-api/error-frame/from-non-gio-error",
      test_error_frame_from_non_gio_error_is_internal);

  return g_test_run ();
}
