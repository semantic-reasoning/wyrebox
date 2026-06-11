#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-message-delivered-payload.h"

static const char *valid_object_key =
    "sha256:"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

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
test_message_delivered_payload_record_round_trip (void)
{
  const guint64 expected_size = 9876543210;
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-message-delivered-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };
  gboolean eof = FALSE;
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_nonnull (root);

  encoded = wyrebox_message_delivered_payload_encode (valid_object_key,
      expected_size, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          encoded, &offset, &sequence, &error));
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
  g_assert_cmpint (record.event_type,
      ==, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  g_assert_cmpuint (record.offset, ==, offset);
  g_assert_cmpuint (record.sequence, ==, sequence);

  g_assert_true (wyrebox_message_delivered_payload_decode (record.payload,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.object_key, ==, valid_object_key);
  g_assert_cmpuint (decoded.size_bytes, ==, expected_size);

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

  g_test_add_func ("/message-delivered-journal/payload-record-round-trip",
      test_message_delivered_payload_record_round_trip);

  return g_test_run ();
}
