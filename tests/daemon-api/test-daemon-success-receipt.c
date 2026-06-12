#include "wyrebox-daemon-success-receipt.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_delivery_ingestion_receipt_copies_journal_identity (void)
{
  WyreboxEmlIngestResult ingest_result = {
    .object_key =
        "sha256:"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    .size_bytes = 12345,
    .journal_offset = 4096,
    .journal_sequence = 7,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };

  g_assert_true (wyrebox_daemon_success_receipt_init_delivery_ingestion
      (&receipt, "request-1", &ingest_result, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (receipt.request_id, ==, "request-1");
  g_assert_cmpstr (receipt.durable_marker, ==, "journal:4096:7");
  g_assert_cmpuint (receipt.journal_offset, ==, 4096);
  g_assert_cmpuint (receipt.journal_sequence, ==, 7);
  g_assert_cmpstr (receipt.summary,
      ==,
      "delivery_ingestion object_key=sha256:"
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "
      "size_bytes=12345");
}

static void
test_delivery_ingestion_receipt_rejects_non_journaled_ingest (void)
{
  WyreboxEmlIngestResult ingest_result = {
    .object_key =
        "sha256:"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    .size_bytes = 12345,
    .journal_offset = 0,
    .journal_sequence = 0,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };

  g_assert_false (wyrebox_daemon_success_receipt_init_delivery_ingestion
      (&receipt, "request-1", &ingest_result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (receipt.request_id);
  g_assert_null (receipt.durable_marker);
  g_assert_cmpuint (receipt.journal_offset, ==, 0);
  g_assert_cmpuint (receipt.journal_sequence, ==, 0);
  g_assert_null (receipt.summary);
}

static void
test_delivery_ingestion_receipt_rejects_missing_request_id (void)
{
  WyreboxEmlIngestResult ingest_result = {
    .object_key =
        "sha256:"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    .size_bytes = 12345,
    .journal_offset = 0,
    .journal_sequence = 1,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };

  g_assert_false (wyrebox_daemon_success_receipt_init_delivery_ingestion
      (&receipt, "", &ingest_result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_mutation_receipt_copies_journal_identity (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_success_receipt_init_fact_mutation (&receipt,
          "request-1", &request, 4096, 7, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (receipt.request_id, ==, "request-1");
  g_assert_cmpstr (receipt.durable_marker, ==, "journal:4096:7");
  g_assert_cmpuint (receipt.journal_offset, ==, 4096);
  g_assert_cmpuint (receipt.journal_sequence, ==, 7);
  g_assert_cmpstr (receipt.summary,
      ==,
      "fact_mutation mutation=retract predicate_id=project_mention "
      "scope_id=account-1 argument_count=2");
}

static void
test_fact_mutation_receipt_rejects_non_journaled_mutation (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_success_receipt_init_fact_mutation (&receipt,
          "request-1", &request, 0, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (receipt.request_id);
  g_assert_null (receipt.durable_marker);
}

static void
test_fact_mutation_receipt_rejects_uninitialized_request (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };

  g_assert_false (wyrebox_daemon_success_receipt_init_fact_mutation (&receipt,
          "request-1", &request, 0, 1, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (receipt.request_id);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/success-receipt/"
      "delivery-ingestion-copies-journal-identity",
      test_delivery_ingestion_receipt_copies_journal_identity);
  g_test_add_func ("/daemon-api/success-receipt/"
      "delivery-ingestion-rejects-non-journaled-ingest",
      test_delivery_ingestion_receipt_rejects_non_journaled_ingest);
  g_test_add_func ("/daemon-api/success-receipt/"
      "delivery-ingestion-rejects-missing-request-id",
      test_delivery_ingestion_receipt_rejects_missing_request_id);
  g_test_add_func ("/daemon-api/success-receipt/"
      "fact-mutation-copies-journal-identity",
      test_fact_mutation_receipt_copies_journal_identity);
  g_test_add_func ("/daemon-api/success-receipt/"
      "fact-mutation-rejects-non-journaled-mutation",
      test_fact_mutation_receipt_rejects_non_journaled_mutation);
  g_test_add_func ("/daemon-api/success-receipt/"
      "fact-mutation-rejects-uninitialized-request",
      test_fact_mutation_receipt_rejects_uninitialized_request);

  return g_test_run ();
}
