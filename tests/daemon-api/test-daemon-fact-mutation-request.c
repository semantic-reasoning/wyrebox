#include "wyrebox-daemon-fact-mutation-request.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

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
test_fact_mutation_request_init_rejects_control_scope (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1\naccount-2", args, &error));
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
test_fact_mutation_request_init_rejects_control_argument (void)
{
  const char *args[] = { "mail-1\nmail-2", NULL };
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

static void
test_fact_mutation_kind_maps_to_journal_event_type (void)
{
  WyreboxJournalEventType event_type = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_fact_mutation_to_event
      (WYREBOX_DAEMON_FACT_MUTATION_INSERT, &event_type, &error));
  g_assert_no_error (error);
  g_assert_cmpint (event_type, ==, WYREBOX_JOURNAL_EVENT_FACT_INSERTED);

  g_assert_true (wyrebox_daemon_fact_mutation_to_event
      (WYREBOX_DAEMON_FACT_MUTATION_RETRACT, &event_type, &error));
  g_assert_no_error (error);
  g_assert_cmpint (event_type, ==, WYREBOX_JOURNAL_EVENT_FACT_RETRACTED);
}

static void
test_fact_mutation_kind_rejects_unknown_journal_event_type (void)
{
  WyreboxJournalEventType event_type = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_fact_mutation_to_event
      ((WyreboxDaemonFactMutationKind) 99, &event_type, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (event_type, ==, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
}

static void
test_fact_mutation_request_gets_journal_event (void)
{
  const char *args[] = { "mail-1", NULL };
  WyreboxJournalEventType event_type = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_fact_mutation_request_get_event
      (&request, &event_type, &error));
  g_assert_no_error (error);
  g_assert_cmpint (event_type, ==, WYREBOX_JOURNAL_EVENT_FACT_RETRACTED);
}

static void
test_fact_mutation_request_rejects_uninitialized_journal_event (void)
{
  WyreboxJournalEventType event_type = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_fact_mutation_request_get_event
      (&request, &event_type, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (event_type, ==, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
}

static void
test_fact_mutation_request_round_trips_journal_payload (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) decoded = { 0 };

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_fact_mutation_request_encode (&request, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_fact_mutation_request_decode (encoded,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_INSERT);
  g_assert_cmpstr (decoded.predicate_id, ==, "project_mention");
  g_assert_cmpstr (decoded.scope_id, ==, "account-1");
  g_assert_cmpstr (decoded.arguments[0], ==, "mail-1");
  g_assert_cmpstr (decoded.arguments[1], ==, "project-a");
  g_assert_null (decoded.arguments[2]);
}

static void
test_fact_mutation_request_encoded_format_matches_golden (void)
{
  const char *args[] = { "mail-1", NULL };
  const guint8 expected[] = {
    'W', 'Y', 'R', 'E', 'F', 'M', 'P', '1',
    0x01,
    0x0f, 0x00, 0x00, 0x00,
    'p', 'r', 'o', 'j', 'e', 'c', 't', '_',
    'm', 'e', 'n', 't', 'i', 'o', 'n',
    0x09, 0x00, 0x00, 0x00,
    'a', 'c', 'c', 'o', 'u', 'n', 't', '-', '1',
    0x01, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00,
    'm', 'a', 'i', 'l', '-', '1',
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_fact_mutation_request_encode (&request, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  encoded_data = g_bytes_get_data (encoded, &encoded_size);
  g_assert_cmpuint (encoded_size, ==, sizeof (expected));
  g_assert_cmpmem (encoded_data, encoded_size, expected, sizeof (expected));
}

static void
test_fact_mutation_request_rejects_uninitialized_encode (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  encoded = wyrebox_daemon_fact_mutation_request_encode (&request, &error);

  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_mutation_request_encode_revalidates_public_fields (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  request.mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT;
  request.predicate_id = g_strdup ("ProjectMention");
  request.scope_id = g_strdup ("account-1");
  request.arguments = g_strdupv ((char **) args);

  encoded = wyrebox_daemon_fact_mutation_request_encode (&request, &error);

  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_mutation_request_rejects_trailing_payload_bytes (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autofree guint8 *copy = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) decoded = { 0 };
  const guint8 *encoded_data = NULL;
  gsize encoded_size = 0;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_fact_mutation_request_encode (&request, &error);
  g_assert_no_error (error);
  encoded_data = g_bytes_get_data (encoded, &encoded_size);

  copy = g_malloc (encoded_size + 1);
  memcpy (copy, encoded_data, encoded_size);
  copy[encoded_size] = 0;
  malformed = g_bytes_new_take (g_steal_pointer (&copy), encoded_size + 1);

  g_assert_false (wyrebox_daemon_fact_mutation_request_decode (malformed,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (decoded.predicate_id);
}

static void
assert_fact_mutation_decode_invalid_data (const guint8 *payload, gsize size)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = g_bytes_new (payload, size);
  g_auto (WyreboxDaemonFactMutationRequest) decoded = { 0 };

  g_assert_false (wyrebox_daemon_fact_mutation_request_decode (encoded,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (decoded.predicate_id);
}

static void
test_fact_mutation_request_rejects_bad_payload_magic (void)
{
  const guint8 payload[] = {
    'B', 'A', 'D', 'F', 'M', 'P', '1', '!',
    WYREBOX_DAEMON_FACT_MUTATION_INSERT,
  };

  assert_fact_mutation_decode_invalid_data (payload, sizeof (payload));
}

static void
test_fact_mutation_request_rejects_bad_payload_mutation (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'F', 'M', 'P', '1',
    99,
  };

  assert_fact_mutation_decode_invalid_data (payload, sizeof (payload));
}

static void
test_fact_mutation_request_rejects_truncated_payload_string (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'F', 'M', 'P', '1',
    WYREBOX_DAEMON_FACT_MUTATION_INSERT,
    5, 0, 0, 0,
    'p', 'r',
  };

  assert_fact_mutation_decode_invalid_data (payload, sizeof (payload));
}

static void
test_fact_mutation_request_rejects_payload_embedded_nul (void)
{
  const guint8 payload[] = {
    'W', 'Y', 'R', 'E', 'F', 'M', 'P', '1',
    WYREBOX_DAEMON_FACT_MUTATION_INSERT,
    3, 0, 0, 0,
    'p', '\0', 'd',
  };

  assert_fact_mutation_decode_invalid_data (payload, sizeof (payload));
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
      "init-rejects-control-scope",
      test_fact_mutation_request_init_rejects_control_scope);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-rejects-null-arguments",
      test_fact_mutation_request_init_rejects_null_arguments);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-rejects-empty-argument",
      test_fact_mutation_request_init_rejects_empty_argument);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "init-rejects-control-argument",
      test_fact_mutation_request_init_rejects_control_argument);
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
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "kind-maps-to-journal-event-type",
      test_fact_mutation_kind_maps_to_journal_event_type);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "kind-rejects-unknown-journal-event-type",
      test_fact_mutation_kind_rejects_unknown_journal_event_type);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "request-gets-journal-event",
      test_fact_mutation_request_gets_journal_event);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "request-rejects-uninitialized-journal-event",
      test_fact_mutation_request_rejects_uninitialized_journal_event);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "round-trips-journal-payload",
      test_fact_mutation_request_round_trips_journal_payload);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "encoded-format-matches-golden",
      test_fact_mutation_request_encoded_format_matches_golden);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "rejects-uninitialized-encode",
      test_fact_mutation_request_rejects_uninitialized_encode);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "encode-revalidates-public-fields",
      test_fact_mutation_request_encode_revalidates_public_fields);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "rejects-trailing-payload-bytes",
      test_fact_mutation_request_rejects_trailing_payload_bytes);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "rejects-bad-payload-magic",
      test_fact_mutation_request_rejects_bad_payload_magic);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "rejects-bad-payload-mutation",
      test_fact_mutation_request_rejects_bad_payload_mutation);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "rejects-truncated-payload-string",
      test_fact_mutation_request_rejects_truncated_payload_string);
  g_test_add_func ("/daemon-api/fact-mutation-request/"
      "rejects-payload-embedded-nul",
      test_fact_mutation_request_rejects_payload_embedded_nul);

  return g_test_run ();
}
