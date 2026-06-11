#include "wyrebox-daemon-response-frame.h"

#include <gio/gio.h>
#include <glib.h>

static WyreboxDaemonSuccessReceipt
make_success_receipt (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .request_id = g_strdup ("request-success"),
    .durable_marker = g_strdup ("journal:0:1"),
    .journal_offset = 0,
    .journal_sequence = 1,
    .summary = g_strdup ("delivery_ingestion object_key=sha256:test "
        "size_bytes=42"),
  };

  return receipt;
}

static void
test_response_frame_init_success_copies_payload (void)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = make_success_receipt ();
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, "correlation-1", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-success");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.success.request_id, ==, "request-success");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");
  g_assert_cmpuint (frame.success.journal_offset, ==, 0);
  g_assert_cmpuint (frame.success.journal_sequence, ==, 1);
  g_assert_cmpstr (frame.success.summary,
      ==, "delivery_ingestion object_key=sha256:test size_bytes=42");
  g_assert_null (frame.error.request_id);
}

static void
test_response_frame_init_error_copies_payload (void)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          "request-error",
          WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE,
          "try later", "retry", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&frame,
          &error_frame, "correlation-2", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-error");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-2");
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.error.request_id, ==, "request-error");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE);
  g_assert_cmpstr (frame.error.message, ==, "try later");
  g_assert_cmpstr (frame.error.retry_hint, ==, "retry");
  g_assert_null (frame.success.request_id);
}

static void
test_response_frame_rejects_invalid_success_payload (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .durable_marker = "journal:0:1",
    .journal_sequence = 1,
    .summary = "missing request id",
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  g_assert_null (frame.request_id);
}

static void
test_response_frame_rejects_non_journaled_success_payload (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .request_id = "request-success",
    .durable_marker = "journal:0:0",
    .journal_sequence = 0,
    .summary = "missing durable journal sequence",
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  g_assert_null (frame.request_id);
}

static void
test_response_frame_rejects_invalid_error_payload (void)
{
  WyreboxDaemonErrorFrame error_frame = {
    .error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
    .message = "missing request id",
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_response_frame_init_error (&frame,
          &error_frame, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  g_assert_null (frame.request_id);
}

static void
test_response_frame_success_then_error_is_mutually_exclusive (void)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = make_success_receipt ();
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, NULL, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          "request-error",
          WYREBOX_DAEMON_ERROR_NOT_FOUND, "not found", NULL, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&frame,
          &error_frame, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-error");
  g_assert_null (frame.success.request_id);
  g_assert_cmpstr (frame.error.request_id, ==, "request-error");
}

static void
test_response_frame_failure_leaves_existing_contents (void)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = make_success_receipt ();
  WyreboxDaemonSuccessReceipt invalid = {
    .durable_marker = "journal:0:1",
    .journal_sequence = 1,
    .summary = "missing request id",
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, "stable-correlation", &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_response_frame_init_success (&frame,
          &invalid, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-success");
  g_assert_cmpstr (frame.correlation_id, ==, "stable-correlation");
  g_assert_cmpstr (frame.success.request_id, ==, "request-success");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/response-frame/success-copies-payload",
      test_response_frame_init_success_copies_payload);
  g_test_add_func ("/daemon-api/response-frame/error-copies-payload",
      test_response_frame_init_error_copies_payload);
  g_test_add_func ("/daemon-api/response-frame/rejects-invalid-success",
      test_response_frame_rejects_invalid_success_payload);
  g_test_add_func ("/daemon-api/response-frame/rejects-non-journaled-success",
      test_response_frame_rejects_non_journaled_success_payload);
  g_test_add_func ("/daemon-api/response-frame/rejects-invalid-error",
      test_response_frame_rejects_invalid_error_payload);
  g_test_add_func ("/daemon-api/response-frame/success-then-error-exclusive",
      test_response_frame_success_then_error_is_mutually_exclusive);
  g_test_add_func ("/daemon-api/response-frame/failure-leaves-existing",
      test_response_frame_failure_leaves_existing_contents);

  return g_test_run ();
}
