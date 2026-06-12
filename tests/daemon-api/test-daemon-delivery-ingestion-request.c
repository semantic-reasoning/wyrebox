#include "wyrebox-daemon-delivery-ingestion-request.h"

#include <gio/gio.h>
#include <string.h>

static void
test_delivery_ingestion_request_copies_fields (void)
{
  g_autoptr (GBytes) message = g_bytes_new_static ("subject: hi\n", 10);
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  const char *recipient_1 = "alice@example.com";
  const char *recipient_2 = "bob@example.com";
  const char *recipients[] = { "alice@example.com", "bob@example.com", NULL };

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-1", "", "", recipients, message, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.delivery_id, ==, "delivery-1");
  g_assert_cmpstr (request.queue_id, ==, "");
  g_assert_cmpstr (request.envelope_sender, ==, "");
  g_assert_cmpstr (request.recipients[0], ==, recipient_1);
  g_assert_cmpstr (request.recipients[1], ==, recipient_2);
  g_assert_null (request.recipients[2]);
  g_assert_cmpuint (g_bytes_get_size (request.message_bytes), ==, 10);
  g_assert_nonnull (request.message_bytes);

  g_assert_false ((const char *const *) request.recipients == recipients);
  g_assert_true (request.recipients[0] != recipient_1);
  g_assert_true (request.recipients[1] != recipient_2);
}

static void
test_delivery_ingestion_request_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  const char *recipients_a[] = { "alice@example.com", NULL };
  const char *recipients_b[] = { "bob@example.com", NULL };
  g_autoptr (GBytes) message_a = g_bytes_new_static ("first", 5);
  g_autoptr (GBytes) message_b = g_bytes_new_static ("second", 6);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-1", "q1", "sender1", recipients_a, message_a, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-2", "q2", NULL, recipients_b, message_b, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.delivery_id, ==, "delivery-2");
  g_assert_cmpstr (request.queue_id, ==, "q2");
  g_assert_null (request.envelope_sender);
  g_assert_cmpstr (request.recipients[0], ==, "bob@example.com");
  g_assert_cmpuint (g_bytes_get_size (request.message_bytes), ==, 6);
}

static void
test_delivery_ingestion_request_rejects_missing_delivery_id (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) message = g_bytes_new_static ("subject: hi\n", 10);
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };

  g_assert_false (wyrebox_daemon_delivery_ingestion_request_init (&request,
          NULL, "q1", "sender", recipients, message, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.delivery_id);
}

static void
test_delivery_ingestion_request_rejects_control_character_in_recipient (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) message = g_bytes_new_static ("subject: hi\n", 10);
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  const char *recipients[] = { "alice\nexample.com", NULL };

  g_assert_false (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-1", "q1", "sender", recipients, message, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.recipients);
}

static void
test_delivery_ingestion_request_rejects_empty_recipients (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) message = g_bytes_new_static ("subject: hi\n", 10);
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-1", "q1", "sender", NULL, message, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.recipients);
}

static void
test_delivery_ingestion_request_rejects_missing_message_bytes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };

  g_assert_false (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-1", "q1", "sender", recipients, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.message_bytes);
}

static void
test_delivery_ingestion_request_rejects_empty_message_bytes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GBytes) message = g_bytes_new_static ("", 0);
  const char *recipients[] = { "alice@example.com", NULL };

  g_assert_false (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-1", "q1", "sender", recipients, message, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_delivery_ingestion_request_rejects_message_bytes_ref_integrity (void)
{
  g_autoptr (GBytes) base_message = g_bytes_new_static ("subject: hi\n", 10);
  g_autoptr (GBytes) copy_message = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;
  const char *recipients[] = { "alice@example.com", NULL };

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-1", "q1", "sender", recipients, base_message, &error));
  g_assert_no_error (error);

  copy_message = g_bytes_new_from_bytes (base_message, 0, 10);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-2", "q2", "sender", recipients, copy_message, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (g_bytes_get_size (request.message_bytes), ==, 10);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/delivery-ingestion-request/copies-fields",
      test_delivery_ingestion_request_copies_fields);
  g_test_add_func ("/daemon-api/delivery-ingestion-request/reinitializes",
      test_delivery_ingestion_request_reinitializes);
  g_test_add_func ("/daemon-api/delivery-ingestion-request/"
      "rejects-missing-delivery-id",
      test_delivery_ingestion_request_rejects_missing_delivery_id);
  g_test_add_func ("/daemon-api/delivery-ingestion-request/"
      "rejects-control-characters",
      test_delivery_ingestion_request_rejects_control_character_in_recipient);
  g_test_add_func ("/daemon-api/delivery-ingestion-request/"
      "rejects-empty-recipients",
      test_delivery_ingestion_request_rejects_empty_recipients);
  g_test_add_func ("/daemon-api/delivery-ingestion-request/"
      "rejects-missing-message-bytes",
      test_delivery_ingestion_request_rejects_missing_message_bytes);
  g_test_add_func ("/daemon-api/delivery-ingestion-request/"
      "rejects-empty-message-bytes",
      test_delivery_ingestion_request_rejects_empty_message_bytes);
  g_test_add_func ("/daemon-api/delivery-ingestion-request/"
      "revalidates-when-message-bytes-replaced",
      test_delivery_ingestion_request_rejects_message_bytes_ref_integrity);

  return g_test_run ();
}
