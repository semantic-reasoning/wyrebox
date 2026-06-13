#include "wyrebox-daemon-audit-payload.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_daemon_audit_payload_roundtrips_single_success (void)
{
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  WyreboxDaemonAuditPayload payload = {
    .operation = WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS,
    .request_id = "request-1",
    .correlation_id = "correlation-1",
    .caller_identity = "trusted-tool",
    .account_identity = "account-1",
    .tool_identity = "fact-importer",
    .scope_id = "account-1",
    .mutation_count = 1,
    .predicate_id = "project_mention",
    .final_journal_offset = 123,
    .final_journal_sequence = 7,
  };

  encoded = wyrebox_daemon_audit_payload_encode (&payload, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_audit_payload_decode (encoded, &decoded,
          &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION);
  g_assert_cmpint (decoded.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS);
  g_assert_cmpstr (decoded.request_id, ==, "request-1");
  g_assert_cmpstr (decoded.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (decoded.caller_identity, ==, "trusted-tool");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "fact-importer");
  g_assert_cmpstr (decoded.scope_id, ==, "account-1");
  g_assert_cmpuint (decoded.mutation_count, ==, 1);
  g_assert_cmpstr (decoded.predicate_id, ==, "project_mention");
  g_assert_cmpuint (decoded.final_journal_offset, ==, 123);
  g_assert_cmpuint (decoded.final_journal_sequence, ==, 7);
}

static void
test_daemon_audit_payload_roundtrips_batch_success (void)
{
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  WyreboxDaemonAuditPayload payload = {
    .operation = WYREBOX_DAEMON_AUDIT_OPERATION_FACT_BATCH_IMPORT,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS,
    .request_id = "request-batch",
    .correlation_id = NULL,
    .caller_identity = "trusted-tool",
    .account_identity = "account-1",
    .tool_identity = NULL,
    .scope_id = "account-1",
    .mutation_count = 2,
    .predicate_id = NULL,
    .final_journal_offset = 456,
    .final_journal_sequence = 3,
  };

  encoded = wyrebox_daemon_audit_payload_encode (&payload, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_audit_payload_decode (encoded, &decoded,
          &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_FACT_BATCH_IMPORT);
  g_assert_cmpuint (decoded.mutation_count, ==, 2);
  g_assert_cmpstr (decoded.request_id, ==, "request-batch");
  g_assert_null (decoded.correlation_id);
  g_assert_null (decoded.tool_identity);
  g_assert_null (decoded.predicate_id);
  g_assert_cmpuint (decoded.final_journal_offset, ==, 456);
  g_assert_cmpuint (decoded.final_journal_sequence, ==, 3);
}

static void
test_daemon_audit_payload_rejects_invalid_encode (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  WyreboxDaemonAuditPayload payload = {
    .operation = WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS,
    .request_id = "",
    .caller_identity = "trusted-tool",
    .account_identity = "account-1",
    .scope_id = "account-1",
    .mutation_count = 1,
    .predicate_id = "project_mention",
    .final_journal_sequence = 1,
  };

  encoded = wyrebox_daemon_audit_payload_encode (&payload, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_daemon_audit_payload_rejects_truncated_decode (void)
{
  const guint8 bytes[] = { 'W', 'Y', 'R', 'E' };
  g_autoptr (GBytes) encoded = g_bytes_new_static (bytes, sizeof (bytes));
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_audit_payload_decode (encoded, &decoded,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_daemon_audit_payload_rejects_trailing_bytes (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) with_trailing = NULL;
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  g_autofree guint8 *copy = NULL;
  gsize size = 0;
  const guint8 *data = NULL;
  WyreboxDaemonAuditPayload payload = {
    .operation = WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS,
    .request_id = "request-1",
    .caller_identity = "trusted-tool",
    .account_identity = "account-1",
    .scope_id = "account-1",
    .mutation_count = 1,
    .predicate_id = "project_mention",
    .final_journal_sequence = 1,
  };

  encoded = wyrebox_daemon_audit_payload_encode (&payload, &error);
  g_assert_no_error (error);
  data = g_bytes_get_data (encoded, &size);
  copy = g_malloc (size + 1);
  memcpy (copy, data, size);
  copy[size] = 0xff;
  with_trailing = g_bytes_new (copy, size + 1);

  g_assert_false (wyrebox_daemon_audit_payload_decode (with_trailing,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/journal/daemon-audit-payload/roundtrip-single-success",
      test_daemon_audit_payload_roundtrips_single_success);
  g_test_add_func ("/journal/daemon-audit-payload/roundtrip-batch-success",
      test_daemon_audit_payload_roundtrips_batch_success);
  g_test_add_func ("/journal/daemon-audit-payload/reject-invalid-encode",
      test_daemon_audit_payload_rejects_invalid_encode);
  g_test_add_func ("/journal/daemon-audit-payload/reject-truncated-decode",
      test_daemon_audit_payload_rejects_truncated_decode);
  g_test_add_func ("/journal/daemon-audit-payload/reject-trailing-bytes",
      test_daemon_audit_payload_rejects_trailing_bytes);

  return g_test_run ();
}
