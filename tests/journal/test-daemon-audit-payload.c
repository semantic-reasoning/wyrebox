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
  {
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);

    g_assert_cmpuint (size, >, 8);
    g_assert_cmpmem (data, 8, "WYREDAU1", 8);
  }

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
  g_assert_null (decoded.query_id);
  g_assert_null (decoded.template_id);
  g_assert_null (decoded.error_domain);
  g_assert_cmpint (decoded.error_code, ==, 0);
  g_assert_null (decoded.error_class);
  g_assert_null (decoded.error_message);
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
test_daemon_audit_payload_decodes_legacy_single_success (void)
{
  static const guint8 legacy_bytes[] = {
    'W', 'Y', 'R', 'E', 'D', 'A', 'U', '1',
    0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x7b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00,
    'r', 'e', 'q', 'u', 'e', 's', 't', '-', '1',
    0x0d, 0x00, 0x00, 0x00,
    'c', 'o', 'r', 'r', 'e', 'l', 'a', 't', 'i', 'o', 'n', '-', '1',
    0x0c, 0x00, 0x00, 0x00,
    't', 'r', 'u', 's', 't', 'e', 'd', '-', 't', 'o', 'o', 'l',
    0x09, 0x00, 0x00, 0x00,
    'a', 'c', 'c', 'o', 'u', 'n', 't', '-', '1',
    0x0d, 0x00, 0x00, 0x00,
    'f', 'a', 'c', 't', '-', 'i', 'm', 'p', 'o', 'r', 't', 'e', 'r',
    0x09, 0x00, 0x00, 0x00,
    'a', 'c', 'c', 'o', 'u', 'n', 't', '-', '1',
    0x0f, 0x00, 0x00, 0x00,
    'p', 'r', 'o', 'j', 'e', 'c', 't', '_', 'm', 'e', 'n', 't', 'i', 'o',
    'n',
  };
  g_autoptr (GBytes) encoded = g_bytes_new_static (legacy_bytes,
      sizeof (legacy_bytes));
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  g_autoptr (GError) error = NULL;

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
  g_assert_null (decoded.query_id);
  g_assert_null (decoded.template_id);
  g_assert_null (decoded.error_domain);
  g_assert_cmpint (decoded.error_code, ==, 0);
  g_assert_null (decoded.error_class);
  g_assert_null (decoded.error_message);
}

static void
test_daemon_audit_payload_roundtrips_duckdb_failure (void)
{
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  WyreboxDaemonAuditPayload payload = {
    .operation = WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE,
    .request_id = "request-query",
    .correlation_id = "correlation-query",
    .caller_identity = "postfix-helper",
    .account_identity = "account-1",
    .tool_identity = "duckdb-tool",
    .scope_id = "account-1",
    .query_id = "query-1",
    .template_id = "mailbox.uid_map.v1",
    .error_domain = "g-io-error-quark",
    .error_code = G_IO_ERROR_PERMISSION_DENIED,
    .error_class = "permissionDenied",
    .error_message = "caller is not authorized",
  };

  encoded = wyrebox_daemon_audit_payload_encode (&payload, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);
  {
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);

    g_assert_cmpuint (size, >, 8);
    g_assert_cmpmem (data, 8, "WYREDAU2", 8);
  }

  g_assert_true (wyrebox_daemon_audit_payload_decode (encoded, &decoded,
          &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE);
  g_assert_cmpint (decoded.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE);
  g_assert_cmpstr (decoded.request_id, ==, "request-query");
  g_assert_cmpstr (decoded.correlation_id, ==, "correlation-query");
  g_assert_cmpstr (decoded.caller_identity, ==, "postfix-helper");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "duckdb-tool");
  g_assert_cmpstr (decoded.scope_id, ==, "account-1");
  g_assert_cmpuint (decoded.mutation_count, ==, 0);
  g_assert_null (decoded.predicate_id);
  g_assert_cmpuint (decoded.final_journal_sequence, ==, 0);
  g_assert_cmpstr (decoded.query_id, ==, "query-1");
  g_assert_cmpstr (decoded.template_id, ==, "mailbox.uid_map.v1");
  g_assert_cmpstr (decoded.error_domain, ==, "g-io-error-quark");
  g_assert_cmpint (decoded.error_code, ==, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpstr (decoded.error_class, ==, "permissionDenied");
  g_assert_cmpstr (decoded.error_message, ==, "caller is not authorized");
}

static void
test_daemon_audit_payload_roundtrips_wirelog_failure (void)
{
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  WyreboxDaemonAuditPayload payload = {
    .operation = WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE,
    .request_id = "request-wirelog",
    .correlation_id = "correlation-wirelog",
    .caller_identity = "trusted-tool",
    .account_identity = "account-1",
    .tool_identity = "wirelog-tool",
    .scope_id = "account-1",
    .query_id = "query-1",
    .template_id = "show_in_virtual_folder.v1",
    .error_domain = "g-io-error-quark",
    .error_code = G_IO_ERROR_INVALID_ARGUMENT,
    .error_class = "permanentFailure",
    .error_message =
        "wirelog predicate query stream chunk must not contain message_id",
  };

  encoded = wyrebox_daemon_audit_payload_encode (&payload, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);
  {
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);

    g_assert_cmpuint (size, >, 8);
    g_assert_cmpmem (data, 8, "WYREDAU2", 8);
  }

  g_assert_true (wyrebox_daemon_audit_payload_decode (encoded, &decoded,
          &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY);
  g_assert_cmpint (decoded.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE);
  g_assert_cmpstr (decoded.request_id, ==, "request-wirelog");
  g_assert_cmpstr (decoded.correlation_id, ==, "correlation-wirelog");
  g_assert_cmpstr (decoded.caller_identity, ==, "trusted-tool");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "wirelog-tool");
  g_assert_cmpstr (decoded.scope_id, ==, "account-1");
  g_assert_cmpuint (decoded.final_journal_sequence, ==, 0);
  g_assert_cmpstr (decoded.query_id, ==, "query-1");
  g_assert_cmpstr (decoded.template_id, ==, "show_in_virtual_folder.v1");
  g_assert_cmpstr (decoded.error_domain, ==, "g-io-error-quark");
  g_assert_cmpint (decoded.error_code, ==, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpstr (decoded.error_class, ==, "permanentFailure");
  g_assert_cmpstr (decoded.error_message, ==,
      "wirelog predicate query stream chunk must not contain message_id");
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
test_daemon_audit_payload_roundtrips_duckdb_success_encode (void)
{
  g_auto (WyreboxDaemonAuditPayload) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  WyreboxDaemonAuditPayload payload = {
    .operation = WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS,
    .request_id = "request-query",
    .correlation_id = "correlation-query",
    .caller_identity = "admin-cli",
    .account_identity = "account-1",
    .tool_identity = "duckdb-tool",
    .scope_id = "account-1",
    .query_id = "query-1",
    .template_id = "mailbox.uid_map.v1",
  };

  encoded = wyrebox_daemon_audit_payload_encode (&payload, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);
  {
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);

    g_assert_cmpuint (size, >, 8);
    g_assert_cmpmem (data, 8, "WYREDAU2", 8);
  }

  g_assert_true (wyrebox_daemon_audit_payload_decode (encoded, &decoded,
          &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE);
  g_assert_cmpint (decoded.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS);
  g_assert_cmpstr (decoded.request_id, ==, "request-query");
  g_assert_cmpstr (decoded.correlation_id, ==, "correlation-query");
  g_assert_cmpstr (decoded.caller_identity, ==, "admin-cli");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "duckdb-tool");
  g_assert_cmpstr (decoded.scope_id, ==, "account-1");
  g_assert_cmpuint (decoded.mutation_count, ==, 0);
  g_assert_cmpuint (decoded.final_journal_sequence, ==, 0);
  g_assert_cmpstr (decoded.query_id, ==, "query-1");
  g_assert_cmpstr (decoded.template_id, ==, "mailbox.uid_map.v1");
  g_assert_null (decoded.error_domain);
  g_assert_cmpint (decoded.error_code, ==, 0);
  g_assert_null (decoded.error_class);
  g_assert_null (decoded.error_message);
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
  g_test_add_func ("/journal/daemon-audit-payload/decode-legacy-single-success",
      test_daemon_audit_payload_decodes_legacy_single_success);
  g_test_add_func ("/journal/daemon-audit-payload/roundtrip-duckdb-failure",
      test_daemon_audit_payload_roundtrips_duckdb_failure);
  g_test_add_func ("/journal/daemon-audit-payload/roundtrip-wirelog-failure",
      test_daemon_audit_payload_roundtrips_wirelog_failure);
  g_test_add_func ("/journal/daemon-audit-payload/reject-invalid-encode",
      test_daemon_audit_payload_rejects_invalid_encode);
  g_test_add_func ("/journal/daemon-audit-payload/roundtrip-duckdb-success",
      test_daemon_audit_payload_roundtrips_duckdb_success_encode);
  g_test_add_func ("/journal/daemon-audit-payload/reject-truncated-decode",
      test_daemon_audit_payload_rejects_truncated_decode);
  g_test_add_func ("/journal/daemon-audit-payload/reject-trailing-bytes",
      test_daemon_audit_payload_rejects_trailing_bytes);

  return g_test_run ();
}
