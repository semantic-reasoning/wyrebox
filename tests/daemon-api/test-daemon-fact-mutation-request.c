#include "wyrebox-daemon-fact-mutation-request.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_fact_mutation_request_init_copies_insert_fields (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  g_assert_cmpint (request.mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_INSERT);
  g_assert_cmpstr (request.predicate_id, ==, "project_mention");
  g_assert_cmpstr (request.scope_id, ==, "account-1");
  g_assert_cmpstr (request.arguments[0], ==, "mail-1");
  g_assert_cmpstr (request.arguments[1], ==, "project-a");
  g_assert_null (request.arguments[2]);
}

static void
test_fact_mutation_request_init_allows_empty_arguments (void)
{
  const char *args[] = { NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
          "_scope_marker", "account-1", args, &error));
  g_assert_no_error (error);

  g_assert_cmpint (request.mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_RETRACT);
  g_assert_cmpstr (request.predicate_id, ==, "_scope_marker");
  g_assert_cmpstr (request.scope_id, ==, "account-1");
  g_assert_null (request.arguments[0]);
}

static void
test_fact_mutation_request_init_rejects_unsupported_kind (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_fact_mutation_request_init (&request,
          (WyreboxDaemonFactMutationKind) 99,
          "project_mention", "account-1", args, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.predicate_id);
}

static void
test_fact_mutation_request_init_rejects_invalid_predicate (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "ProjectMention", "account-1", args, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.predicate_id);
}

static void
test_fact_mutation_request_init_rejects_missing_scope (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "", args, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.scope_id);
}

static void
test_fact_mutation_request_init_rejects_null_arguments (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.arguments);
}

static void
test_fact_mutation_request_init_rejects_empty_argument (void)
{
  const char *args[] = { "mail-1", "", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.arguments);
}

static void
test_fact_mutation_request_init_replaces_existing_value (void)
{
  const char *first_args[] = { "mail-1", NULL };
  const char *second_args[] = { "mail-2", "project-b", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "old_fact", "account-1", first_args, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
          "project_mention", "account-2", second_args, &error));
  g_assert_no_error (error);

  g_assert_cmpint (request.mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_RETRACT);
  g_assert_cmpstr (request.predicate_id, ==, "project_mention");
  g_assert_cmpstr (request.scope_id, ==, "account-2");
  g_assert_cmpstr (request.arguments[0], ==, "mail-2");
  g_assert_cmpstr (request.arguments[1], ==, "project-b");
  g_assert_null (request.arguments[2]);
}

static void
test_fact_mutation_kind_converts_to_wire_name (void)
{
  g_assert_cmpstr (wyrebox_daemon_fact_mutation_kind_to_wire_name
      (WYREBOX_DAEMON_FACT_MUTATION_INSERT), ==, "insert");
  g_assert_cmpstr (wyrebox_daemon_fact_mutation_kind_to_wire_name
      (WYREBOX_DAEMON_FACT_MUTATION_RETRACT), ==, "retract");
  g_assert_null (wyrebox_daemon_fact_mutation_kind_to_wire_name (
          (WyreboxDaemonFactMutationKind) 99));
}

static void
test_fact_mutation_kind_parses_wire_name (void)
{
  WyreboxDaemonFactMutationKind mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT;
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_fact_mutation_kind_from_wire_name ("retract",
          &mutation, &error));
  g_assert_no_error (error);
  g_assert_cmpint (mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_RETRACT);
}

static void
test_fact_mutation_kind_rejects_unknown_wire_name (void)
{
  WyreboxDaemonFactMutationKind mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT;
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_fact_mutation_kind_from_wire_name ("delete",
          &mutation, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_INSERT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-copies-insert-fields",
      test_fact_mutation_request_init_copies_insert_fields);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-allows-empty-arguments",
      test_fact_mutation_request_init_allows_empty_arguments);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-rejects-unsupported-kind",
      test_fact_mutation_request_init_rejects_unsupported_kind);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-rejects-invalid-predicate",
      test_fact_mutation_request_init_rejects_invalid_predicate);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-rejects-missing-scope",
      test_fact_mutation_request_init_rejects_missing_scope);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-rejects-null-arguments",
      test_fact_mutation_request_init_rejects_null_arguments);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-rejects-empty-argument",
      test_fact_mutation_request_init_rejects_empty_argument);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-replaces-existing-value",
      test_fact_mutation_request_init_replaces_existing_value);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "kind-converts-to-wire-name",
      test_fact_mutation_kind_converts_to_wire_name);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "kind-parses-wire-name", test_fact_mutation_kind_parses_wire_name);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "kind-rejects-unknown-wire-name",
      test_fact_mutation_kind_rejects_unknown_wire_name);

  return g_test_run ();
}
