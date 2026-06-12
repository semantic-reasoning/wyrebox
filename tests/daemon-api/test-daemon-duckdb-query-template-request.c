#include "wyrebox-daemon-duckdb-query-template-request.h"

#include <gio/gio.h>

static void
test_duckdb_query_template_request_copies_fields (void)
{
  const char *parameters[] = { "mail-1", "project-a", NULL };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (&request,
          "query-1", "template.summary", "account-1", parameters, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.query_id, ==, "query-1");
  g_assert_cmpstr (request.template_id, ==, "template.summary");
  g_assert_cmpstr (request.scope_id, ==, "account-1");
  g_assert_cmpstr (request.parameters[0], ==, "mail-1");
  g_assert_cmpstr (request.parameters[1], ==, "project-a");
  g_assert_null (request.parameters[2]);
}

static void
test_duckdb_query_template_request_allows_empty_parameters (void)
{
  const char *parameters[] = { NULL };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (&request,
          "query-1", "template.summary", "account-1", parameters, &error));
  g_assert_no_error (error);
  g_assert_nonnull (request.parameters);
  g_assert_null (request.parameters[0]);
}

static void
test_duckdb_query_template_request_reinitializes (void)
{
  const char *first_parameters[] = { "mail-1", NULL };
  const char *second_parameters[] = { "mail-2", NULL };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (&request,
          "query-1", "template.summary", "account-1", first_parameters,
          &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (&request,
          "query-2", "template.detail", "account-2", second_parameters,
          &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.query_id, ==, "query-2");
  g_assert_cmpstr (request.template_id, ==, "template.detail");
  g_assert_cmpstr (request.scope_id, ==, "account-2");
  g_assert_cmpstr (request.parameters[0], ==, "mail-2");
  g_assert_null (request.parameters[1]);
}

static void
assert_init_rejected (const char *query_id,
    const char *template_id, const char *scope_id,
    const char *const *parameters)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_duckdb_query_template_request_init (&request,
          query_id, template_id, scope_id, parameters, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.query_id);
  g_assert_null (request.template_id);
  g_assert_null (request.scope_id);
  g_assert_null (request.parameters);
}

static void
test_duckdb_query_template_request_rejects_missing_query_id (void)
{
  const char *parameters[] = { NULL };

  assert_init_rejected ("", "template.summary", "account-1", parameters);
}

static void
test_duckdb_query_template_request_rejects_invalid_query_id (void)
{
  const char *parameters[] = { NULL };

  assert_init_rejected ("query;drop", "template.summary", "account-1",
      parameters);
}

static void
test_duckdb_query_template_request_rejects_missing_template (void)
{
  const char *parameters[] = { NULL };

  assert_init_rejected ("query-1", NULL, "account-1", parameters);
}

static void
test_duckdb_query_template_request_rejects_invalid_template (void)
{
  const char *parameters[] = { NULL };

  assert_init_rejected ("query-1", "project mention", "account-1", parameters);
}

static void
test_duckdb_query_template_request_rejects_missing_scope (void)
{
  const char *parameters[] = { NULL };

  assert_init_rejected ("query-1", "template.summary", "", parameters);
}

static void
test_duckdb_query_template_request_rejects_scope_control_char (void)
{
  const char *parameters[] = { NULL };

  assert_init_rejected ("query-1", "template.summary", "account\n1",
      parameters);
}

static void
test_duckdb_query_template_request_rejects_missing_parameters (void)
{
  assert_init_rejected ("query-1", "template.summary", "account-1", NULL);
}

static void
test_duckdb_query_template_request_rejects_empty_parameter (void)
{
  const char *parameters[] = { "", NULL };

  assert_init_rejected ("query-1", "template.summary", "account-1", parameters);
}

static void
test_duckdb_query_template_request_rejects_parameter_control_char (void)
{
  const char *parameters[] = { "mail\n1", NULL };

  assert_init_rejected ("query-1", "template.summary", "account-1", parameters);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/duckdb-query-template/request/copies-fields",
      test_duckdb_query_template_request_copies_fields);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/allows-empty-parameters",
      test_duckdb_query_template_request_allows_empty_parameters);
  g_test_add_func ("/daemon-api/duckdb-query-template/request/reinitializes",
      test_duckdb_query_template_request_reinitializes);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-missing-query-id",
      test_duckdb_query_template_request_rejects_missing_query_id);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-invalid-query-id",
      test_duckdb_query_template_request_rejects_invalid_query_id);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-missing-template",
      test_duckdb_query_template_request_rejects_missing_template);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-invalid-template",
      test_duckdb_query_template_request_rejects_invalid_template);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-missing-scope",
      test_duckdb_query_template_request_rejects_missing_scope);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-scope-control-char",
      test_duckdb_query_template_request_rejects_scope_control_char);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-missing-parameters",
      test_duckdb_query_template_request_rejects_missing_parameters);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-empty-parameter",
      test_duckdb_query_template_request_rejects_empty_parameter);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/request/rejects-parameter-control-char",
      test_duckdb_query_template_request_rejects_parameter_control_char);

  return g_test_run ();
}
