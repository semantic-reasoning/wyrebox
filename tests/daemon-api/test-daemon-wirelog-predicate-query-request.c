#include "wyrebox-daemon-wirelog-predicate-query-request.h"

#include <gio/gio.h>

static void
test_wirelog_predicate_query_request_copies_fields (void)
{
  const char *bindings[] = { "mail-1", "project-a", NULL };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init (&request,
          "query-1", "project_mention", "account-1", bindings, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.query_id, ==, "query-1");
  g_assert_cmpstr (request.predicate_id, ==, "project_mention");
  g_assert_cmpstr (request.scope_id, ==, "account-1");
  g_assert_cmpstr (request.bindings[0], ==, "mail-1");
  g_assert_cmpstr (request.bindings[1], ==, "project-a");
  g_assert_null (request.bindings[2]);
}

static void
test_wirelog_predicate_query_request_allows_empty_bindings (void)
{
  const char *bindings[] = { NULL };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init (&request,
          "query-1", "project_mention", "account-1", bindings, &error));
  g_assert_no_error (error);
  g_assert_nonnull (request.bindings);
  g_assert_null (request.bindings[0]);
}

static void
test_wirelog_predicate_query_request_reinitializes (void)
{
  const char *first_bindings[] = { "mail-1", NULL };
  const char *second_bindings[] = { "mail-2", NULL };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init (&request,
          "query-1", "project_mention", "account-1", first_bindings, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init (&request,
          "query-2", "sender_belongs_to", "account-2", second_bindings,
          &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.query_id, ==, "query-2");
  g_assert_cmpstr (request.predicate_id, ==, "sender_belongs_to");
  g_assert_cmpstr (request.scope_id, ==, "account-2");
  g_assert_cmpstr (request.bindings[0], ==, "mail-2");
  g_assert_null (request.bindings[1]);
}

static void
assert_init_rejected (const char *query_id,
    const char *predicate_id, const char *scope_id, const char *const *bindings)
{
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_wirelog_predicate_query_request_init (&request,
          query_id, predicate_id, scope_id, bindings, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.query_id);
  g_assert_null (request.predicate_id);
  g_assert_null (request.scope_id);
  g_assert_null (request.bindings);
}

static void
test_wirelog_predicate_query_request_rejects_missing_query_id (void)
{
  const char *bindings[] = { NULL };

  assert_init_rejected ("", "project_mention", "account-1", bindings);
}

static void
test_wirelog_predicate_query_request_rejects_invalid_query_id (void)
{
  const char *bindings[] = { NULL };

  assert_init_rejected ("query;drop", "project_mention", "account-1", bindings);
}

static void
test_wirelog_predicate_query_request_rejects_missing_predicate (void)
{
  const char *bindings[] = { NULL };

  assert_init_rejected ("query-1", NULL, "account-1", bindings);
}

static void
test_wirelog_predicate_query_request_rejects_invalid_predicate (void)
{
  const char *bindings[] = { NULL };

  assert_init_rejected ("query-1", "project mention", "account-1", bindings);
}

static void
test_wirelog_predicate_query_request_rejects_missing_scope (void)
{
  const char *bindings[] = { NULL };

  assert_init_rejected ("query-1", "project_mention", "", bindings);
}

static void
test_wirelog_predicate_query_request_rejects_scope_control_char (void)
{
  const char *bindings[] = { NULL };

  assert_init_rejected ("query-1", "project_mention", "account\n1", bindings);
}

static void
test_wirelog_predicate_query_request_rejects_missing_bindings (void)
{
  assert_init_rejected ("query-1", "project_mention", "account-1", NULL);
}

static void
test_wirelog_predicate_query_request_rejects_empty_binding (void)
{
  const char *bindings[] = { "", NULL };

  assert_init_rejected ("query-1", "project_mention", "account-1", bindings);
}

static void
test_wirelog_predicate_query_request_rejects_binding_control_char (void)
{
  const char *bindings[] = { "mail\n1", NULL };

  assert_init_rejected ("query-1", "project_mention", "account-1", bindings);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/wirelog-predicate-query/request/copies-fields",
      test_wirelog_predicate_query_request_copies_fields);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/allows-empty-bindings",
      test_wirelog_predicate_query_request_allows_empty_bindings);
  g_test_add_func ("/daemon-api/wirelog-predicate-query/request/reinitializes",
      test_wirelog_predicate_query_request_reinitializes);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-missing-query-id",
      test_wirelog_predicate_query_request_rejects_missing_query_id);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-invalid-query-id",
      test_wirelog_predicate_query_request_rejects_invalid_query_id);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-missing-predicate",
      test_wirelog_predicate_query_request_rejects_missing_predicate);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-invalid-predicate",
      test_wirelog_predicate_query_request_rejects_invalid_predicate);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-missing-scope",
      test_wirelog_predicate_query_request_rejects_missing_scope);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-scope-control-char",
      test_wirelog_predicate_query_request_rejects_scope_control_char);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-missing-bindings",
      test_wirelog_predicate_query_request_rejects_missing_bindings);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-empty-binding",
      test_wirelog_predicate_query_request_rejects_empty_binding);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/request/rejects-binding-control-char",
      test_wirelog_predicate_query_request_rejects_binding_control_char);

  return g_test_run ();
}
