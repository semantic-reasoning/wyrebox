#include "wyrebox-daemon-wirelog-predicate-query-catalog.h"

#include <gio/gio.h>

static void
init_request (WyreboxDaemonWirelogPredicateQueryRequest *request,
    const char *predicate_id, const char *const *bindings)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init (request,
          "query-1", predicate_id, "account-1", bindings, &error));
  g_assert_no_error (error);
}

static void
test_catalog_resolves_allowlisted_predicate (void)
{
  const char *bindings[] = { NULL };
  const WyreboxDaemonWirelogPredicateQueryDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "show_in_virtual_folder.v1", bindings);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
          &descriptor, &error));
  g_assert_no_error (error);
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->predicate_id, ==, "show_in_virtual_folder.v1");
  g_assert_cmpstr (descriptor->relation_name, ==, "show_in_virtual_folder");
  g_assert_cmpstr (descriptor->scope_kind, ==, "account_id");
  g_assert_cmpstr (descriptor->output_format, ==,
      "stream-chunk.wirelog-predicate.show-in-virtual-folder.v1");
  g_assert_cmpuint (descriptor->n_bindings, ==, 0);
}

static void
test_catalog_allows_trusted_tool (void)
{
  const char *bindings[] = { NULL };
  const WyreboxDaemonWirelogPredicateQueryDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "show_in_virtual_folder.v1", bindings);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL, "account-1", &request,
          &descriptor, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (descriptor->predicate_id, ==, "show_in_virtual_folder.v1");
}

static void
test_catalog_rejects_unauthorized_clients (void)
{
  const char *bindings[] = { NULL };
  WyreboxDaemonClientIdentityClass classes[] = {
    WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER,
    WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN,
    WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN,
  };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };

  init_request (&request, "show_in_virtual_folder.v1", bindings);

  for (gsize i = 0; i < G_N_ELEMENTS (classes); i++) {
    const WyreboxDaemonWirelogPredicateQueryDescriptor *descriptor = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_false (wyrebox_daemon_wirelog_predicate_query_catalog_validate
        (classes[i], "account-1", &request, &descriptor, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_null (descriptor);
  }
}

static void
test_catalog_rejects_unknown_predicate (void)
{
  const char *bindings[] = { NULL };
  const WyreboxDaemonWirelogPredicateQueryDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "unknown_predicate.v1", bindings);

  g_assert_false (wyrebox_daemon_wirelog_predicate_query_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (descriptor);
}

static void
test_catalog_rejects_wrong_binding_count (void)
{
  const char *bindings[] = { "message-1", NULL };
  const WyreboxDaemonWirelogPredicateQueryDescriptor *descriptor = NULL;
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "show_in_virtual_folder.v1", bindings);

  g_assert_false (wyrebox_daemon_wirelog_predicate_query_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request,
          &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (descriptor);
}

static void
test_catalog_rejects_bad_binding_text (void)
{
  char *bindings[] = { "", NULL };
  WyreboxDaemonWirelogPredicateQueryRequest request = {
    (char *) "query-1",
    (char *) "show_in_virtual_folder.v1",
    (char *) "account-1",
    bindings,
  };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_wirelog_predicate_query_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request, NULL,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  bindings[0] = "message\n1";
  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_wirelog_predicate_query_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-1", &request, NULL,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_catalog_rejects_missing_or_mismatched_scope (void)
{
  const char *bindings[] = { NULL };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_request (&request, "show_in_virtual_folder.v1", bindings);

  g_assert_false (wyrebox_daemon_wirelog_predicate_query_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "", &request, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_wirelog_predicate_query_catalog_validate
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI, "account-2", &request, NULL,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/catalog/resolves-allowlisted",
      test_catalog_resolves_allowlisted_predicate);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/catalog/allows-trusted-tool",
      test_catalog_allows_trusted_tool);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/catalog/rejects-unauthorized-clients",
      test_catalog_rejects_unauthorized_clients);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/catalog/rejects-unknown-predicate",
      test_catalog_rejects_unknown_predicate);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/catalog/rejects-wrong-binding-count",
      test_catalog_rejects_wrong_binding_count);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/catalog/rejects-bad-binding-text",
      test_catalog_rejects_bad_binding_text);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/catalog/rejects-scope",
      test_catalog_rejects_missing_or_mismatched_scope);

  return g_test_run ();
}
