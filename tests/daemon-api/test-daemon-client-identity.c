#include "wyrebox-daemon-client-identity.h"

#include <glib.h>

static void
test_classifies_supported_names (void)
{
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_name ("admin-cli"),
      ==, WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI);
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_name
      ("trusted-tool"), ==, WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL);
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_name
      ("postfix-helper"), ==, WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER);
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_name
      ("dovecot-plugin"), ==, WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN);
}

static void
test_unknown_names_are_unauthorized (void)
{
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_name (NULL),
      ==, WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN);
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_name (""), ==,
      WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN);
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_name ("skill"),
      ==, WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN);
  g_assert_false (wyrebox_daemon_client_identity_can_query_controlled_views
      (WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN));
}

static void
test_classifies_request_identity (void)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", "account-1", "tool-1",
          "correlation-1", &error));
  g_assert_no_error (error);
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_request (&identity),
      ==, WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL);
  g_assert_cmpint (wyrebox_daemon_client_identity_classify_request (NULL),
      ==, WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN);
}

static void
test_controlled_query_authorization_classes (void)
{
  g_assert_true (wyrebox_daemon_client_identity_can_query_controlled_views
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI));
  g_assert_true (wyrebox_daemon_client_identity_can_query_controlled_views
      (WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL));
  g_assert_false (wyrebox_daemon_client_identity_can_query_controlled_views
      (WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER));
  g_assert_false (wyrebox_daemon_client_identity_can_query_controlled_views
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN));
}

static void
test_fact_mutation_authorization_classes (void)
{
  g_assert_true (wyrebox_daemon_client_identity_can_mutate_facts
      (WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL));
  g_assert_false (wyrebox_daemon_client_identity_can_mutate_facts
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI));
  g_assert_false (wyrebox_daemon_client_identity_can_mutate_facts
      (WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER));
  g_assert_false (wyrebox_daemon_client_identity_can_mutate_facts
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN));
  g_assert_false (wyrebox_daemon_client_identity_can_mutate_facts
      (WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN));
}

static void
test_class_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_daemon_client_identity_class_to_name
      (WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI), ==, "admin-cli");
  g_assert_cmpstr (wyrebox_daemon_client_identity_class_to_name
      (WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL), ==, "trusted-tool");
  g_assert_cmpstr (wyrebox_daemon_client_identity_class_to_name
      (WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER), ==, "postfix-helper");
  g_assert_cmpstr (wyrebox_daemon_client_identity_class_to_name
      (WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN), ==, "dovecot-plugin");
  g_assert_cmpstr (wyrebox_daemon_client_identity_class_to_name
      (WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN), ==, "unknown");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/client-identity/classifies-supported",
      test_classifies_supported_names);
  g_test_add_func ("/daemon-api/client-identity/unknown-unauthorized",
      test_unknown_names_are_unauthorized);
  g_test_add_func ("/daemon-api/client-identity/classifies-request",
      test_classifies_request_identity);
  g_test_add_func ("/daemon-api/client-identity/query-authorization",
      test_controlled_query_authorization_classes);
  g_test_add_func ("/daemon-api/client-identity/fact-mutation-authorization",
      test_fact_mutation_authorization_classes);
  g_test_add_func ("/daemon-api/client-identity/class-names",
      test_class_names_are_stable);

  return g_test_run ();
}
