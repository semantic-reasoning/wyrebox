#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"

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

static void
test_fact_mutation_request_appends_journal_record (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) decoded = { 0 };
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_daemon_fact_mutation_request_append_journal (&request,
          writer, &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (offset, ==, 0);
  g_assert_cmpuint (sequence, ==, 1);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);
  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpint (record.event_type, ==, WYREBOX_JOURNAL_EVENT_FACT_RETRACTED);
  g_assert_cmpuint (record.offset, ==, offset);
  g_assert_cmpuint (record.sequence, ==, sequence);

  g_assert_true (wyrebox_daemon_fact_mutation_request_decode (record.payload,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_RETRACT);
  g_assert_cmpstr (decoded.predicate_id, ==, "project_mention");
  g_assert_cmpstr (decoded.scope_id, ==, "account-1");
  g_assert_cmpstr (decoded.arguments[0], ==, "mail-1");
  g_assert_cmpstr (decoded.arguments[1], ==, "project-a");
  g_assert_null (decoded.arguments[2]);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/fact-mutation-journal/request-appends-record",
      test_fact_mutation_request_appends_journal_record);

  return g_test_run ();
}
