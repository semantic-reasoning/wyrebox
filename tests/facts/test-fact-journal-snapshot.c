#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-fact-journal-snapshot.h"
#include "wyrebox-journal-writer.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static char *
make_journal_root (void)
{
  g_autoptr (GError) error = NULL;
  char *root = NULL;

  root = g_dir_make_tmp ("wyrebox-fact-journal-snapshot-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (root);
  return root;
}

static void
append_mutation (WyreboxJournalWriter *writer,
    WyreboxDaemonFactMutationKind mutation,
    const char *predicate,
    const char *scope, const char *const *args, guint64 *out_sequence)
{
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          mutation, predicate, scope, args, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_append_journal (&request,
          writer, &offset, &sequence, &error));
  g_assert_no_error (error);

  if (out_sequence != NULL)
    *out_sequence = sequence;
}

static void
append_unrelated_event (WyreboxJournalWriter *writer)
{
  const guint8 payload[] = { 0x01, 0x02, 0x03 };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, bytes, &offset, &sequence,
          &error));
  g_assert_no_error (error);
}

static void
append_malformed_fact_event (WyreboxJournalWriter *writer)
{
  const guint8 payload[] = { 0xff };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_FACT_INSERTED, bytes, &offset, &sequence,
          &error));
  g_assert_no_error (error);
}

static GPtrArray *
load_snapshot (const char *root)
{
  g_autoptr (GError) error = NULL;
  GPtrArray *records = NULL;

  records = wyrebox_fact_journal_snapshot_load_active (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (records);
  return records;
}

static const WyreboxFactRecord *
record_at (GPtrArray *records, guint index)
{
  g_assert_nonnull (records);
  g_assert_cmpuint (index, <, records->len);
  return g_ptr_array_index (records, index);
}

static void
assert_record (const WyreboxFactRecord *record,
    const char *predicate,
    const char *source,
    const char *first_arg, const char *second_arg, guint64 created_at_unix_us)
{
  g_assert_cmpstr (record->predicate, ==, predicate);
  g_assert_cmpstr (record->source, ==, source);
  g_assert_cmpuint (record->confidence_ppm, ==, 1000000);
  g_assert_cmpuint (record->created_at_unix_us, ==, created_at_unix_us);
  g_assert_cmpuint (record->retracted_at_unix_us, ==, 0);
  g_assert_cmpstr (record->args[0], ==, first_arg);
  if (second_arg != NULL) {
    g_assert_cmpstr (record->args[1], ==, second_arg);
    g_assert_null (record->args[2]);
  } else {
    g_assert_null (record->args[1]);
  }
}

static void
test_inserted_fact_appears (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GError) error = NULL;
  guint64 sequence = 0;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_keyword", "account-1", args, &sequence);

  records = load_snapshot (root);
  g_assert_cmpuint (records->len, ==, 1);
  assert_record (record_at (records, 0), "project_keyword",
      "fact-mutation:account-1", "mail-1", "project-a", sequence);

  remove_tree (root);
}

static void
test_retracted_fact_absent (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GError) error = NULL;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_keyword", "account-1", args, NULL);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
      "project_keyword", "account-1", args, NULL);

  records = load_snapshot (root);
  g_assert_cmpuint (records->len, ==, 0);

  remove_tree (root);
}

static void
test_insert_retract_insert_uses_later_sequence (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GError) error = NULL;
  guint64 reinsert_sequence = 0;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_keyword", "account-1", args, NULL);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
      "project_keyword", "account-1", args, NULL);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_keyword", "account-1", args, &reinsert_sequence);

  records = load_snapshot (root);
  g_assert_cmpuint (records->len, ==, 1);
  assert_record (record_at (records, 0), "project_keyword",
      "fact-mutation:account-1", "mail-1", "project-a", reinsert_sequence);

  remove_tree (root);
}

static void
test_duplicate_insert_is_idempotent (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GError) error = NULL;
  guint64 first_sequence = 0;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_keyword", "account-1", args, &first_sequence);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_keyword", "account-1", args, NULL);

  records = load_snapshot (root);
  g_assert_cmpuint (records->len, ==, 1);
  assert_record (record_at (records, 0), "project_keyword",
      "fact-mutation:account-1", "mail-1", "project-a", first_sequence);

  remove_tree (root);
}

static void
test_missing_retract_is_noop (void)
{
  const char *missing_args[] = { "mail-missing", "project-a", NULL };
  const char *active_args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GError) error = NULL;
  guint64 active_sequence = 0;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
      "project_keyword", "account-1", missing_args, NULL);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_keyword", "account-1", active_args, &active_sequence);

  records = load_snapshot (root);
  g_assert_cmpuint (records->len, ==, 1);
  assert_record (record_at (records, 0), "project_keyword",
      "fact-mutation:account-1", "mail-1", "project-a", active_sequence);

  remove_tree (root);
}

static void
test_unrelated_events_are_ignored (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GError) error = NULL;
  guint64 sequence = 0;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_unrelated_event (writer);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_keyword", "account-1", args, &sequence);
  append_unrelated_event (writer);

  records = load_snapshot (root);
  g_assert_cmpuint (records->len, ==, 1);
  assert_record (record_at (records, 0), "project_keyword",
      "fact-mutation:account-1", "mail-1", "project-a", sequence);

  remove_tree (root);
}

static void
test_output_is_sorted_deterministically (void)
{
  const char *z_args[] = { "mail-z", NULL };
  const char *a_args[] = { "mail-a", NULL };
  const char *b_args[] = { "mail-b", NULL };
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GError) error = NULL;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "z_predicate", "scope-b", z_args, NULL);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "a_predicate", "scope-b", b_args, NULL);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "a_predicate", "scope-a", z_args, NULL);
  append_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "a_predicate", "scope-a", a_args, NULL);

  records = load_snapshot (root);
  g_assert_cmpuint (records->len, ==, 4);
  assert_record (record_at (records, 0), "a_predicate",
      "fact-mutation:scope-a", "mail-a", NULL, 4);
  assert_record (record_at (records, 1), "a_predicate",
      "fact-mutation:scope-a", "mail-z", NULL, 3);
  assert_record (record_at (records, 2), "a_predicate",
      "fact-mutation:scope-b", "mail-b", NULL, 2);
  assert_record (record_at (records, 3), "z_predicate",
      "fact-mutation:scope-b", "mail-z", NULL, 1);

  remove_tree (root);
}

static void
test_malformed_fact_payload_errors (void)
{
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GError) error = NULL;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_malformed_fact_event (writer);

  records = wyrebox_fact_journal_snapshot_load_active (root, &error);
  g_assert_null (records);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/facts/fact-journal-snapshot/inserted-appears",
      test_inserted_fact_appears);
  g_test_add_func ("/facts/fact-journal-snapshot/retracted-absent",
      test_retracted_fact_absent);
  g_test_add_func ("/facts/fact-journal-snapshot/reinsert-sequence",
      test_insert_retract_insert_uses_later_sequence);
  g_test_add_func ("/facts/fact-journal-snapshot/duplicate-insert",
      test_duplicate_insert_is_idempotent);
  g_test_add_func ("/facts/fact-journal-snapshot/missing-retract",
      test_missing_retract_is_noop);
  g_test_add_func ("/facts/fact-journal-snapshot/unrelated-ignored",
      test_unrelated_events_are_ignored);
  g_test_add_func ("/facts/fact-journal-snapshot/sorted-output",
      test_output_is_sorted_deterministically);
  g_test_add_func ("/facts/fact-journal-snapshot/malformed-payload",
      test_malformed_fact_payload_errors);

  return g_test_run ();
}
