#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"

#define SEGMENT_NAME "00000000000000000000.wbj"
#define JOURNAL_MAGIC "WYREJNL1"

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
segment_path_for_root (const char *root)
{
  return g_build_filename (root, SEGMENT_NAME, NULL);
}

static void
read_segment (const char *path, guint8 **out_data, gsize *out_size)
{
  g_autofree gchar *contents = NULL;

  g_file_get_contents (path, &contents, out_size, NULL);
  g_assert_nonnull (contents);
  *out_data = (guint8 *) g_steal_pointer (&contents);
}

static void
write_u16_le (guint8 *buffer, guint16 value)
{
  buffer[0] = (guint8) ((value >> 0) & 0xFF);
  buffer[1] = (guint8) ((value >> 8) & 0xFF);
}

static void
write_u32_le (guint8 *buffer, guint32 value)
{
  buffer[0] = (guint8) ((value >> 0) & 0xFF);
  buffer[1] = (guint8) ((value >> 8) & 0xFF);
  buffer[2] = (guint8) ((value >> 16) & 0xFF);
  buffer[3] = (guint8) ((value >> 24) & 0xFF);
}

static void
write_u64_le (guint8 *buffer, guint64 value)
{
  buffer[0] = (guint8) ((value >> 0) & 0xFF);
  buffer[1] = (guint8) ((value >> 8) & 0xFF);
  buffer[2] = (guint8) ((value >> 16) & 0xFF);
  buffer[3] = (guint8) ((value >> 24) & 0xFF);
  buffer[4] = (guint8) ((value >> 32) & 0xFF);
  buffer[5] = (guint8) ((value >> 40) & 0xFF);
  buffer[6] = (guint8) ((value >> 48) & 0xFF);
  buffer[7] = (guint8) ((value >> 56) & 0xFF);
}

static void
compute_checksum (const char *event_type,
    guint64 sequence,
    gsize payload_len, const guint8 *payload, gsize digest_size, guint8 *digest)
{
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  gsize computed_len = digest_size;
  guint8 header[32] = { 0 };
  const gsize event_len = strlen (event_type);

  memcpy (header, JOURNAL_MAGIC, strlen (JOURNAL_MAGIC) + 1);
  write_u16_le (header + 8, 64);
  write_u16_le (header + 10, 1);
  write_u32_le (header + 12, (guint32) event_len);
  write_u64_le (header + 16, sequence);
  write_u64_le (header + 24, (guint64) payload_len);

  g_checksum_update (checksum, header, sizeof (header));
  g_checksum_update (checksum, (const guchar *) event_type, (gssize) event_len);
  if (payload_len > 0) {
    g_checksum_update (checksum, (const guchar *) payload,
        (gssize) payload_len);
  }

  g_checksum_get_digest (checksum, digest, &computed_len);
  g_assert_cmpuint (computed_len, ==, digest_size);
}

static void
overwrite_segment (const char *path, guint8 *data, gsize size)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_set_contents (path, (const gchar *) data, (gssize) size,
          &error));
  g_assert_no_error (error);
}

static void
test_reads_one_record (void)
{
  const guint8 payload[] = { 0x68, 0x69, 0x0a };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-reader-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));
  guint64 out_offset = 0;
  guint64 out_sequence = 0;
  gsize payload_size = 0;
  const guint8 *payload_data = NULL;

  g_assert_nonnull (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload_bytes, &out_offset, &out_sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (out_offset, ==, 0);
  g_assert_cmpuint (out_sequence, ==, 1);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpuint (record.offset, ==, out_offset);
  g_assert_cmpuint (record.sequence, ==, 1);
  g_assert_cmpint (record.event_type,
      ==, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  payload_data = g_bytes_get_data (record.payload, &payload_size);
  g_assert_cmpuint (payload_size, ==, sizeof (payload));
  g_assert_cmpmem (payload_data, payload_size, payload, sizeof (payload));

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (root);
}

static void
test_reads_multiple_records_and_second_offset (void)
{
  const guint8 first_payload[] = { 0x6f, 0x6e, 0x65 };
  const guint8 second_payload[] = { 0x74, 0x77, 0x6f };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-reader-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GBytes) first_bytes = g_bytes_new_static (first_payload,
      sizeof (first_payload));
  g_autoptr (GBytes) second_bytes = g_bytes_new_static (second_payload,
      sizeof (second_payload));
  g_auto (WyreboxJournalRecord) first_record = { 0 };
  g_auto (WyreboxJournalRecord) second_record = { 0 };
  gboolean eof = FALSE;
  guint64 first_offset = 0;
  guint64 second_offset = 0;
  guint64 first_sequence = 0;
  guint64 second_sequence = 0;
  gsize first_payload_size = 0;
  gsize second_payload_size = 0;
  const guint8 *first_payload_data = NULL;
  const guint8 *second_payload_data = NULL;

  g_assert_nonnull (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          first_bytes, &first_offset, &first_sequence, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          second_bytes, &second_offset, &second_sequence, &error));
  g_assert_no_error (error);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_nonnull (reader);
  g_assert_no_error (error);

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &first_record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpuint (first_record.offset, ==, first_offset);
  g_assert_cmpuint (first_record.sequence, ==, 1);

  first_payload_data =
      g_bytes_get_data (first_record.payload, &first_payload_size);
  g_assert_cmpuint (first_payload_size, ==, sizeof (first_payload));
  g_assert_cmpmem (first_payload_data,
      first_payload_size, first_payload, sizeof (first_payload));

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &second_record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpuint (second_record.offset, ==, second_offset);
  g_assert_cmpuint (second_record.sequence, ==, 2);

  second_payload_data =
      g_bytes_get_data (second_record.payload, &second_payload_size);
  g_assert_cmpuint (second_payload_size, ==, sizeof (second_payload));
  g_assert_cmpmem (second_payload_data,
      second_payload_size, second_payload, sizeof (second_payload));

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &first_record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (root);
}

static void
test_clean_eof_on_empty_root_or_segment (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-reader-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  g_assert_nonnull (root);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_nonnull (reader);
  g_assert_no_error (error);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  segment_path = segment_path_for_root (root);
  g_assert_true (g_file_set_contents (segment_path, "", -1, &error));
  g_assert_no_error (error);

  g_clear_object (&reader);
  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_nonnull (reader);
  g_assert_no_error (error);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (root);
}

static void
test_rejects_invalid_magic (void)
{
  const guint8 payload[] = { 0x62, 0x61, 0x64 };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-reader-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;
  guint64 sequence = 0;
  guint64 offset = 0;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));
  gboolean eof = FALSE;

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload_bytes, &offset, &sequence, &error));
  g_assert_no_error (error);

  read_segment (segment_path, &segment, &segment_size);
  g_assert_cmpuint (segment_size, >, 0);
  segment[0] ^= 0xFF;

  overwrite_segment (segment_path, segment, segment_size);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_nonnull (reader);
  g_assert_no_error (error);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  remove_tree (root);
}

static void
test_rejects_invalid_checksum (void)
{
  const guint8 payload[] = { 0x63, 0x68, 0x65, 0x63, 0x6b };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-reader-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;
  guint64 sequence = 0;
  guint64 offset = 0;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));
  gboolean eof = FALSE;

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload_bytes, &offset, &sequence, &error));
  g_assert_no_error (error);

  read_segment (segment_path, &segment, &segment_size);
  g_assert_cmpuint (segment_size, >, 64 + 15 + sizeof (payload));
  segment[50] ^= 0x01;

  overwrite_segment (segment_path, segment, segment_size);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_nonnull (reader);
  g_assert_no_error (error);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  remove_tree (root);
}

static void
test_rejects_truncated_record (void)
{
  const guint8 payload[] = { 0x74, 0x72, 0x75, 0x6e, 0x63 };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-reader-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;
  guint64 sequence = 0;
  guint64 offset = 0;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));
  gboolean eof = FALSE;

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload_bytes, &offset, &sequence, &error));
  g_assert_no_error (error);

  read_segment (segment_path, &segment, &segment_size);
  g_assert_cmpuint (segment_size, >, 16);
  overwrite_segment (segment_path, segment, segment_size - 1);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_nonnull (reader);
  g_assert_no_error (error);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  remove_tree (root);
}

static void
test_rejects_non_monotonic_sequence (void)
{
  const guint8 first_payload[] = { 0x6d, 0x6f, 0x72, 0x65 };
  const guint8 second_payload[] = { 0x76, 0x69, 0x6f, 0x6c, 0x65, 0x74 };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-reader-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autofree guint8 *segment = NULL;
  g_autofree guint8 *checksum = NULL;
  gsize segment_size = 0;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GBytes) first_bytes = g_bytes_new_static (first_payload,
      sizeof (first_payload));
  g_autoptr (GBytes) second_bytes = g_bytes_new_static (second_payload,
      sizeof (second_payload));
  gsize second_offset = 0;
  guint64 first_offset = 0;
  guint64 second_sequence = 0;
  guint64 first_sequence = 0;
  gboolean eof = FALSE;
  gsize checksum_len = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  const char *event_type =
      wyrebox_journal_event_type_to_string
      (WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  guint32 event_len = 0;
  gsize payload_len = sizeof (second_payload);

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          first_bytes, &first_offset, &first_sequence, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          second_bytes, &second_offset, &second_sequence, &error));
  g_assert_no_error (error);

  read_segment (segment_path, &segment, &segment_size);
  event_len = (guint32) strlen (event_type);
  second_offset = 64 + event_len + sizeof (first_payload);
  g_assert_cmpuint (segment_size, >=,
      second_offset + 64 + event_len + payload_len);

  write_u64_le (segment + second_offset + 16, 1);
  checksum = g_malloc (checksum_len);
  compute_checksum (event_type,
      1, payload_len, second_payload, checksum_len, checksum);
  memcpy (segment + second_offset + 32, checksum, checksum_len);

  overwrite_segment (segment_path, segment, segment_size);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_nonnull (reader);
  g_assert_no_error (error);

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  remove_tree (root);
}

static void
test_rejects_unknown_event_type (void)
{
  const guint8 payload[] = { 0x75, 0x6e, 0x6b, 0x6e, 0x6f, 0x77, 0x6e };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-reader-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;
  guint64 sequence = 0;
  guint64 offset = 0;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_autofree guint8 *checksum = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));
  const char *event_type =
      wyrebox_journal_event_type_to_string
      (WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  const char *unknown_event_type = "NotARealMsgTypeX";
  gsize checksum_len = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  gboolean eof = FALSE;

  g_assert_nonnull (root);
  g_assert_cmpuint (strlen (event_type), ==, strlen (unknown_event_type));
  segment_path = segment_path_for_root (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload_bytes, &offset, &sequence, &error));
  g_assert_no_error (error);

  read_segment (segment_path, &segment, &segment_size);
  memcpy (segment + 64, unknown_event_type, strlen (unknown_event_type));

  checksum = g_malloc (checksum_len);
  compute_checksum (unknown_event_type,
      1, sizeof (payload), payload, checksum_len, checksum);
  memcpy (segment + 32, checksum, checksum_len);

  overwrite_segment (segment_path, segment, segment_size);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_nonnull (reader);
  g_assert_no_error (error);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/journal-reader/reads-one-record", test_reads_one_record);
  g_test_add_func ("/journal-reader/reads-multiple-records-and-offset",
      test_reads_multiple_records_and_second_offset);
  g_test_add_func ("/journal-reader/clean-eof-on-empty-root-or-segment",
      test_clean_eof_on_empty_root_or_segment);
  g_test_add_func ("/journal-reader/rejects-invalid-magic",
      test_rejects_invalid_magic);
  g_test_add_func ("/journal-reader/rejects-invalid-checksum",
      test_rejects_invalid_checksum);
  g_test_add_func ("/journal-reader/rejects-truncated-record",
      test_rejects_truncated_record);
  g_test_add_func ("/journal-reader/rejects-non-monotonic-sequence",
      test_rejects_non_monotonic_sequence);
  g_test_add_func ("/journal-reader/rejects-unknown-event-type",
      test_rejects_unknown_event_type);

  return g_test_run ();
}
