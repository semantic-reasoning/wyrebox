#include "wyrebox-daemon-duckdb-query-template-catalog.h"

#include <gio/gio.h>

static void
init_request (WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const char *template_id, const char *const *parameters)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", template_id, "account-1", parameters, &error));
  g_assert_no_error (error);
}

static void
assert_template_resolves (const char *template_id, const char *parameter,
    const char *expected_name, const char *expected_parameter_name,
    const char *expected_output_format)
{
  const char *parameters[] = { parameter, NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, template_id, parameters);

  g_assert_true (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
          &descriptor, &error));
  g_assert_no_error (error);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->template_id, ==, template_id);
  g_assert_cmpstr (descriptor->name, ==, expected_name);
  g_assert_cmpstr (descriptor->scope_kind, ==, "account_id");
  g_assert_cmpstr (descriptor->output_format, ==, expected_output_format);
  g_assert_cmpuint (descriptor->n_parameters, ==, 1);
  g_assert_cmpstr (descriptor->parameter_names[0], ==, expected_parameter_name);
  g_assert_null (descriptor->parameter_names[1]);
}

static void
assert_metadata_template_resolves (const char *template_id,
    const char *parameter, const char *expected_name,
    const char *expected_parameter_name, const char *expected_output_format)
{
  const char *parameters[] = { parameter, "100", "0", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, template_id, parameters);

  g_assert_true (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
          &descriptor, &error));
  g_assert_no_error (error);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->template_id, ==, template_id);
  g_assert_cmpstr (descriptor->name, ==, expected_name);
  g_assert_cmpstr (descriptor->scope_kind, ==, "account_id");
  g_assert_cmpstr (descriptor->output_format, ==, expected_output_format);
  g_assert_cmpuint (descriptor->n_parameters, ==, 3);
  g_assert_cmpstr (descriptor->parameter_names[0], ==, expected_parameter_name);
  g_assert_cmpstr (descriptor->parameter_names[1], ==, "limit");
  g_assert_cmpstr (descriptor->parameter_names[2], ==, "offset");
  g_assert_null (descriptor->parameter_names[3]);
}

static void
test_catalog_resolves_allowlisted_templates (void)
{
  const char *date_range_parameters[] = { "1704067200000000",
    "1704240000000000", "100", "0", NULL
  };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) date_range_request = { 0 };
  g_autoptr (GError) error = NULL;

  assert_template_resolves ("mailbox.uid_map.v1", "mailbox-inbox",
      "mailbox uid map", "mailbox_id",
      "stream-chunk.duckdb-template.uid-map.v1");
  assert_template_resolves ("derived_view.uid_map.v1", "view-important",
      "derived view uid map", "view_id",
      "stream-chunk.duckdb-template.derived-view-uid-map.v1");
  assert_template_resolves ("message.by_id.v1", "message-1",
      "message by id", "message_id",
      "stream-chunk.duckdb-template.message-by-id.v1");
  assert_template_resolves ("facts.by_source.v1", "rule",
      "facts by source", "source",
      "stream-chunk.duckdb-template.facts-by-source.v1");
  assert_template_resolves ("facts.by_fact_id.v1", "fact-a",
      "facts by fact id", "fact_id",
      "stream-chunk.duckdb-template.facts-by-fact-id.v1");
  assert_template_resolves ("facts.by_fact_id_with_provenance.v1", "fact-a",
      "facts by fact id with provenance", "fact_id",
      "stream-chunk.duckdb-template.facts-by-fact-id-with-provenance.v1");
  assert_template_resolves ("mailbox.history_by_message.v1", "message-a",
      "mailbox history by message", "message_id",
      "stream-chunk.duckdb-template.mailbox-history-by-message.v1");
  assert_metadata_template_resolves ("messages.by_from_addr.v1",
      "Alice <alice@example.test>", "messages by from address", "from_addr",
      "stream-chunk.duckdb-template.messages-by-from-addr.v1");
  assert_metadata_template_resolves ("messages.by_sender_domain.v1",
      "example.test",
      "messages by sender domain", "sender_domain",
      "stream-chunk.duckdb-template.messages-by-sender-domain.v1");
  assert_metadata_template_resolves ("messages.by_subject.v1", "Subject A",
      "messages by subject", "subject",
      "stream-chunk.duckdb-template.messages-by-subject.v1");
  assert_metadata_template_resolves ("messages.subject_contains.v1",
      "subject", "messages subject contains", "subject_term",
      "stream-chunk.duckdb-template.messages-subject-contains.v1");

  init_request (&date_range_request, "messages.by_date_range.v1",
      date_range_parameters);
  g_assert_true (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1",
          &date_range_request, &descriptor, &error));
  g_assert_no_error (error);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->template_id, ==, "messages.by_date_range.v1");
  g_assert_cmpstr (descriptor->name, ==, "messages by date range");
  g_assert_cmpstr (descriptor->scope_kind, ==, "account_id");
  g_assert_cmpstr (descriptor->output_format, ==,
      "stream-chunk.duckdb-template.messages-by-date-range.v1");
  g_assert_cmpuint (descriptor->n_parameters, ==, 4);
  g_assert_cmpstr (descriptor->parameter_names[0], ==, "start_unix_us");
  g_assert_cmpstr (descriptor->parameter_names[1], ==, "end_unix_us");
  g_assert_cmpstr (descriptor->parameter_names[2], ==, "limit");
  g_assert_cmpstr (descriptor->parameter_names[3], ==, "offset");
  g_assert_null (descriptor->parameter_names[4]);
}

static void
test_catalog_allows_trusted_tool (void)
{
  const char *parameters[] = { "mailbox-inbox", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "mailbox.uid_map.v1", parameters);

  g_assert_true (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL, "account-1", &request,
          &descriptor, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (descriptor->template_id, ==, "mailbox.uid_map.v1");
}

static void
test_catalog_allows_dovecot_uid_map_templates (void)
{
  const char *mailbox_parameters[] = { "mailbox-inbox", NULL };
  const char *derived_view_parameters[] = { "view-important", NULL };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) mailbox_request = { 0 };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) derived_view_request = {
    0
  };
  g_autoptr (GError) error = NULL;
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;

  init_request (&mailbox_request, "mailbox.uid_map.v1", mailbox_parameters);
  init_request (&derived_view_request, "derived_view.uid_map.v1",
      derived_view_parameters);

  g_assert_true (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN, "account-1",
          &mailbox_request, &descriptor, &error));
  g_assert_no_error (error);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->template_id, ==, "mailbox.uid_map.v1");

  g_clear_error (&error);
  descriptor = NULL;
  g_assert_true (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN, "account-1",
          &derived_view_request, &descriptor, &error));
  g_assert_no_error (error);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->template_id, ==, "derived_view.uid_map.v1");
}

static void
test_catalog_rejects_unauthorized_clients (void)
{
  const char *parameters[] = { "mailbox-inbox", NULL };
  WyreboxDaemonClientIdentityClass classes[] = {
    WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER,
    WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN,
  };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };

  init_request (&request, "mailbox.uid_map.v1", parameters);

  for (gsize i = 0; i < G_N_ELEMENTS (classes); i++) {
    const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
        (classes[i], "account-1", &request, &descriptor, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_null (descriptor);
  }
}

static void
test_catalog_rejects_dovecot_for_search_templates (void)
{
  const char *parameters[] = { "subject", "100", "0", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "messages.subject_contains.v1", parameters);

  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_null (descriptor);
}

static void
test_catalog_rejects_dovecot_for_fact_provenance_templates (void)
{
  const char *parameters[] = { "rule", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "facts.by_source.v1", parameters);

  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_null (descriptor);

  init_request (&request, "facts.by_fact_id.v1", parameters);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_null (descriptor);

  init_request (&request, "facts.by_fact_id_with_provenance.v1", parameters);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_null (descriptor);

  init_request (&request, "mailbox.history_by_message.v1", parameters);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_null (descriptor);
}

static void
test_catalog_rejects_unknown_template (void)
{
  const char *parameters[] = { "mailbox-inbox", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "unknown.template.v1", parameters);

  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (descriptor);
}

static void
test_catalog_rejects_wrong_parameter_count (void)
{
  const char *parameters[] = { "mailbox-inbox", "extra", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "mailbox.uid_map.v1", parameters);

  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (descriptor);
}

static void
test_catalog_rejects_bad_parameter_text (void)
{
  char *parameters[] = { "", NULL };
  WyreboxDaemonDuckDBQueryTemplateRequest request = {
    (char *) "query-1",
    (char *) "mailbox.uid_map.v1",
    (char *) "account-1",
    parameters,
  };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request, NULL,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  parameters[0] = "mailbox\ninbox";
  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request, NULL,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_date_range_validation (const gchar *start_unix_us,
    const gchar *end_unix_us, gboolean expected_valid)
{
  const char *parameters[] = { start_unix_us, end_unix_us, "100", "0", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "messages.by_date_range.v1", parameters);

  if (expected_valid) {
    g_assert_true (wyrebox_daemon_duckdb_query_template_catalog_validate
        (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
            &descriptor, &error));
    g_assert_no_error (error);
    g_assert_nonnull (descriptor);
    g_assert_cmpstr (descriptor->template_id, ==, "messages.by_date_range.v1");
  } else {
    g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
        (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
            &descriptor, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null (descriptor);
  }
}

static void
assert_metadata_limit_offset_validation (const gchar *limit,
    const gchar *offset, gboolean expected_valid)
{
  const char *parameters[] = { "Subject A", limit, offset, NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "messages.by_subject.v1", parameters);

  if (expected_valid) {
    g_assert_true (wyrebox_daemon_duckdb_query_template_catalog_validate
        (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
            &descriptor, &error));
    g_assert_no_error (error);
    g_assert_nonnull (descriptor);
    g_assert_cmpstr (descriptor->template_id, ==, "messages.by_subject.v1");
  } else {
    g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
        (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
            &descriptor, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null (descriptor);
  }
}

static void
test_catalog_validates_date_range_integer_bounds (void)
{
  assert_date_range_validation ("-9223372036854775808", "0", TRUE);
  assert_date_range_validation ("0", "9223372036854775807", TRUE);
  assert_date_range_validation ("-2", "-1", TRUE);
  assert_date_range_validation ("0", "0", TRUE);
  assert_date_range_validation ("1", "2", TRUE);
  assert_date_range_validation ("9223372036854775807", "0", TRUE);
  assert_date_range_validation ("abc", "1", FALSE);
  assert_date_range_validation ("1", "abc", FALSE);
  assert_date_range_validation ("123abc", "1", FALSE);
  assert_date_range_validation ("+1", "2", FALSE);
  assert_date_range_validation (" 1", "2", FALSE);
  assert_date_range_validation ("1 ", "2", FALSE);
  assert_date_range_validation ("1.5", "2", FALSE);
  assert_date_range_validation ("1e6", "2", FALSE);
  assert_date_range_validation ("0); DROP TABLE messages; --", "1", FALSE);
  assert_date_range_validation ("9223372036854775808", "1", FALSE);
  assert_date_range_validation ("-9223372036854775809", "1", FALSE);
}

static void
test_catalog_validates_metadata_limit_offset_bounds (void)
{
  assert_metadata_limit_offset_validation ("1", "0", TRUE);
  assert_metadata_limit_offset_validation ("100", "9223372036854775807", TRUE);
  assert_metadata_limit_offset_validation ("0", "0", FALSE);
  assert_metadata_limit_offset_validation ("101", "0", FALSE);
  assert_metadata_limit_offset_validation ("-1", "0", FALSE);
  assert_metadata_limit_offset_validation ("1", "-1", FALSE);
  assert_metadata_limit_offset_validation ("1.5", "0", FALSE);
  assert_metadata_limit_offset_validation ("1", "0); DROP TABLE messages; --",
      FALSE);
  assert_metadata_limit_offset_validation ("18446744073709551616", "0", FALSE);
  assert_metadata_limit_offset_validation ("1", "9223372036854775808", FALSE);
  assert_metadata_limit_offset_validation ("1", "18446744073709551615", FALSE);
}

static void
assert_metadata_template_rejects_empty_term (const gchar *template_id)
{
  char *parameters[] = { "", "100", "0", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  WyreboxDaemonDuckDBQueryTemplateRequest request = {
    (char *) "query-1",
    (char *) template_id,
    (char *) "account-1",
    parameters,
  };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (descriptor);
}

static void
test_catalog_rejects_empty_metadata_search_terms (void)
{
  assert_metadata_template_rejects_empty_term ("messages.by_from_addr.v1");
  assert_metadata_template_rejects_empty_term ("messages.by_sender_domain.v1");
  assert_metadata_template_rejects_empty_term ("messages.by_subject.v1");
  assert_metadata_template_rejects_empty_term ("messages.subject_contains.v1");
}

static void
test_catalog_rejects_missing_or_mismatched_scope (void)
{
  const char *parameters[] = { "mailbox-inbox", NULL };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "mailbox.uid_map.v1", parameters);

  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "", &request, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_duckdb_query_template_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-2", &request, NULL,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/resolves-allowlisted",
      test_catalog_resolves_allowlisted_templates);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/allows-trusted-tool",
      test_catalog_allows_trusted_tool);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-unauthorized-clients",
      test_catalog_rejects_unauthorized_clients);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/allows-dovecot-uid-map-templates",
      test_catalog_allows_dovecot_uid_map_templates);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-dovecot-for-search-templates",
      test_catalog_rejects_dovecot_for_search_templates);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-dovecot-for-fact-provenance-templates",
      test_catalog_rejects_dovecot_for_fact_provenance_templates);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-unknown-template",
      test_catalog_rejects_unknown_template);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-wrong-parameter-count",
      test_catalog_rejects_wrong_parameter_count);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-bad-parameter-text",
      test_catalog_rejects_bad_parameter_text);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/validates-date-range-integer-bounds",
      test_catalog_validates_date_range_integer_bounds);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/validates-metadata-limit-offset-bounds",
      test_catalog_validates_metadata_limit_offset_bounds);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-empty-metadata-search-terms",
      test_catalog_rejects_empty_metadata_search_terms);
  g_test_add_func ("/daemon-api/duckdb-query-template/catalog/rejects-scope",
      test_catalog_rejects_missing_or_mismatched_scope);

  return g_test_run ();
}
