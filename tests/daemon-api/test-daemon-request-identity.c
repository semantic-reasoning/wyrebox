#include "wyrebox-daemon-request-identity.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_request_identity_init_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          "postfix-pipe",
          "account-1", "wyrebox-postfix-pipe", "queue-abc", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (identity.request_id, ==, "request-1");
  g_assert_cmpstr (identity.caller_identity, ==, "postfix-pipe");
  g_assert_cmpstr (identity.account_identity, ==, "account-1");
  g_assert_cmpstr (identity.tool_identity, ==, "wyrebox-postfix-pipe");
  g_assert_cmpstr (identity.correlation_id, ==, "queue-abc");
}

static void
test_request_identity_init_allows_optional_correlation_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (identity.request_id, ==, "request-1");
  g_assert_null (identity.caller_identity);
  g_assert_null (identity.account_identity);
  g_assert_null (identity.tool_identity);
  g_assert_null (identity.correlation_id);
}

static void
test_request_identity_init_rejects_missing_request_id (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };

  g_assert_false (wyrebox_daemon_request_identity_init (&identity,
          "", "postfix-pipe", NULL, NULL, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (identity.request_id);
}

static void
test_request_identity_init_replaces_existing_value (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "caller-1", NULL, NULL, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (identity.request_id, ==, "request-1");
  g_assert_cmpstr (identity.caller_identity, ==, "caller-1");

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-2", NULL, "account-2", NULL, "corr-2", &error));
  g_assert_no_error (error);
  g_assert_cmpstr (identity.request_id, ==, "request-2");
  g_assert_null (identity.caller_identity);
  g_assert_cmpstr (identity.account_identity, ==, "account-2");
  g_assert_null (identity.tool_identity);
  g_assert_cmpstr (identity.correlation_id, ==, "corr-2");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/request-identity/init-copies-fields",
      test_request_identity_init_copies_fields);
  g_test_add_func ("/daemon-api/request-identity/"
      "init-allows-optional-correlation-fields",
      test_request_identity_init_allows_optional_correlation_fields);
  g_test_add_func ("/daemon-api/request-identity/"
      "init-rejects-missing-request-id",
      test_request_identity_init_rejects_missing_request_id);
  g_test_add_func ("/daemon-api/request-identity/"
      "init-replaces-existing-value",
      test_request_identity_init_replaces_existing_value);

  return g_test_run ();
}
