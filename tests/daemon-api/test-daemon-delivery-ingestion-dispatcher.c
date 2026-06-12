#include "wyrebox-daemon-delivery-ingestion-dispatcher.h"
#include "wyrebox-daemon-delivery-ingestion-request.h"

#include <gio/gio.h>
#include <glib.h>

static gboolean
ingest_delivery_success (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-delivery");
  g_assert_cmpstr (identity->caller_identity, ==, "postfix");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "postfix");
  g_assert_cmpstr (request->delivery_id, ==, "delivery-123");
  g_assert_cmpstr (request->recipients[0], ==, "alice@example.com");
  g_assert_cmpuint (g_bytes_get_size (request->message_bytes), ==, 12);

  if (was_called != NULL)
    *was_called = TRUE;

  out_result->object_key =
      g_strdup
      ("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  out_result->size_bytes = 10;
  out_result->journal_offset = 4096;
  out_result->journal_sequence = 7;
  return TRUE;
}

static gboolean
fail_delivery_without_error (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;
  (void) identity;
  (void) request;
  (void) out_result;

  if (was_called != NULL)
    *was_called = TRUE;

  return FALSE;
}

static gboolean
ingest_delivery_without_object_key (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;
  (void) identity;
  (void) request;

  if (was_called != NULL)
    *was_called = TRUE;

  out_result->object_key = NULL;
  out_result->size_bytes = 10;
  out_result->journal_offset = 0;
  out_result->journal_sequence = 0;
  return TRUE;
}

static void
test_dispatcher_handles_valid_envelope (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) message = g_bytes_new_static ("message-bytes", 12);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123", "queue-1", NULL, recipients, message, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_delivery_ingestion_service_new
      (ingest_delivery_success, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "postfix",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-delivery");
  g_assert_cmpstr (frame.correlation_id, ==, "postfix-ingest-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:4096:7");
}

static void
test_dispatcher_rejects_unauthorized_caller_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) message = g_bytes_new_static ("message-bytes", 12);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123", "queue-1", NULL, recipients, message, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_delivery_ingestion_service_new
      (ingest_delivery_success, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "skill",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_dispatcher_converts_silent_failure_to_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) message = g_bytes_new_static ("message-bytes", 12);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123", "queue-1", NULL, recipients, message, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_delivery_ingestion_service_new
      (fail_delivery_without_error, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "postfix",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

static void
test_dispatcher_rejects_invalid_delivery_result (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) message = g_bytes_new_static ("message-bytes", 12);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123", "queue-1", NULL, recipients, message, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_delivery_ingestion_service_new
      (ingest_delivery_without_object_key, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "postfix",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "handles-valid-envelope", test_dispatcher_handles_valid_envelope);
  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "rejects-unauthorized-caller-with-error-frame",
      test_dispatcher_rejects_unauthorized_caller_with_error_frame);
  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "converts-silent-failure-to-error-frame",
      test_dispatcher_converts_silent_failure_to_error_frame);
  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "rejects-invalid-delivery-result",
      test_dispatcher_rejects_invalid_delivery_result);

  return g_test_run ();
}
