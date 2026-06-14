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
test_catalog_resolves_allowlisted_templates (void)
{
  assert_template_resolves ("mailbox.uid_map.v1", "mailbox-inbox",
      "mailbox uid map", "mailbox_id",
      "stream-chunk.duckdb-template.uid-map.v1");
  assert_template_resolves ("derived_view.uid_map.v1", "view-important",
      "derived view uid map", "view_id",
      "stream-chunk.duckdb-template.derived-view-uid-map.v1");
  assert_template_resolves ("message.by_id.v1", "message-1",
      "message by id", "message_id",
      "stream-chunk.duckdb-template.message-by-id.v1");
  assert_template_resolves ("messages.by_from_addr.v1",
      "Alice <alice@example.test>", "messages by from address", "from_addr",
      "stream-chunk.duckdb-template.messages-by-from-addr.v1");
  assert_template_resolves ("messages.by_sender_domain.v1", "example.test",
      "messages by sender domain", "sender_domain",
      "stream-chunk.duckdb-template.messages-by-sender-domain.v1");
  assert_template_resolves ("messages.by_subject.v1", "Subject A",
      "messages by subject", "subject",
      "stream-chunk.duckdb-template.messages-by-subject.v1");
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
  const char *parameters[] = { "message-1", NULL };
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "message.by_id.v1", parameters);

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
      ("/daemon-api/duckdb-query-template/catalog/rejects-unknown-template",
      test_catalog_rejects_unknown_template);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-wrong-parameter-count",
      test_catalog_rejects_wrong_parameter_count);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/catalog/rejects-bad-parameter-text",
      test_catalog_rejects_bad_parameter_text);
  g_test_add_func ("/daemon-api/duckdb-query-template/catalog/rejects-scope",
      test_catalog_rejects_missing_or_mismatched_scope);

  return g_test_run ();
}
