#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"

#define SEGMENT_NAME "00000000000000000000.wbj"
#define JOURNAL_MAGIC "WYREJNL1"
#define CONCURRENT_APPEND_THREADS 8
#define CONCURRENT_APPEND_RECORDS_PER_THREAD 16

static const char *canonical_event_types[] = {
  "MessageDelivered",
  "FlagChanged",
  "KeywordChanged",
  "FactInserted",
  "FactRetracted",
  "DerivedViewMembershipChanged",
  "DaemonAuditRecorded",
};

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

static guint16
read_u16_le (const guint8 *buffer)
{
  return (guint16) (buffer[0] | ((guint16) buffer[1] << 8));
}

static guint32
read_u32_le (const guint8 *buffer)
{
  return (guint32) (buffer[0] |
      ((guint32) buffer[1] << 8) |
      ((guint32) buffer[2] << 16) | ((guint32) buffer[3] << 24));
}

static guint64
read_u64_le (const guint8 *buffer)
{
  return (guint64) buffer[0] |
      ((guint64) buffer[1] << 8) |
      ((guint64) buffer[2] << 16) |
      ((guint64) buffer[3] << 24) |
      ((guint64) buffer[4] << 32) |
      ((guint64) buffer[5] << 40) |
      ((guint64) buffer[6] << 48) | ((guint64) buffer[7] << 56);
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

  memcpy (header, JOURNAL_MAGIC, strlen (JOURNAL_MAGIC));
  write_u16_le (header + 8, 64);
  write_u16_le (header + 10, 1);
  write_u32_le (header + 12, (guint32) event_len);
  write_u64_le (header + 16, sequence);
  write_u64_le (header + 24, (guint64) payload_len);

  g_checksum_update (checksum, header, sizeof (header));
  g_checksum_update (checksum, (const guchar *) event_type, (gssize) event_len);
  if (payload_len > 0)
    g_checksum_update (checksum,
        (const guchar *) payload, (gssize) payload_len);

  g_checksum_get_digest (checksum, digest, &computed_len);
  g_assert_cmpuint (computed_len, ==, digest_size);
}

static void
read_segment (const char *path, guint8 **out_data, gsize *out_size)
{
  g_autofree char *contents = NULL;

  g_file_get_contents (path, &contents, out_size, NULL);
  g_assert_nonnull (contents);
  *out_data = (guint8 *) g_steal_pointer (&contents);
}

static void
assert_record_at (const guint8 *segment,
    gsize segment_size,
    gsize offset,
    guint64 expected_sequence,
    const char *expected_event_type,
    const guint8 *expected_payload, gsize expected_payload_len)
{
  const guint8 *cursor = segment + offset;
  const gsize event_len = strlen (expected_event_type);
  const gsize expected_size = 64 + event_len + expected_payload_len;

  g_assert_cmpuint (segment_size, >=, offset + expected_size);
  g_assert_cmpmem (cursor, 8, JOURNAL_MAGIC, 8);
  g_assert_cmpuint (read_u16_le (cursor + 8), ==, 64);
  g_assert_cmpuint (read_u16_le (cursor + 10), ==, 1);
  g_assert_cmpuint (read_u32_le (cursor + 12), ==, event_len);
  g_assert_cmpuint (read_u64_le (cursor + 16), ==, expected_sequence);
  g_assert_cmpuint (read_u64_le (cursor + 24), ==, expected_payload_len);

  const guint8 *payload = expected_payload == NULL ? (const guint8 *) "" :
      expected_payload;

  g_assert_cmpmem (cursor + 64, event_len, expected_event_type, event_len);
  g_assert_cmpmem (cursor + 64 + event_len,
      expected_payload_len, payload, expected_payload_len);

  g_autofree guint8 *expected_checksum = NULL;
  gsize checksum_len = g_checksum_type_get_length (G_CHECKSUM_SHA256);

  g_assert_cmpuint (checksum_len, ==, 32);
  expected_checksum = g_malloc (checksum_len);

  compute_checksum (expected_event_type,
      expected_sequence,
      expected_payload_len, payload, checksum_len, expected_checksum);

  g_assert_cmpmem (cursor + 32, 32, expected_checksum, 32);
}

static void
test_new_creates_empty_segment (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-writer-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  GStatBuf segment_stat = { 0 };

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (g_file_test (segment_path, G_FILE_TEST_EXISTS));
  g_assert_cmpint (g_stat (segment_path, &segment_stat), ==, 0);
  g_assert_cmpint (segment_stat.st_size, ==, 0);

  remove_tree (root);
}

static void
test_appends_message_delivered_record (void)
{
  const guint8 payload[] = { 0x68, 0x69, 0x0a };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-writer-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;
  guint64 out_offset = 0;
  guint64 out_sequence = 0;

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload_bytes, &out_offset, &out_sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (out_offset, ==, 0);
  g_assert_cmpuint (out_sequence, ==, 1);

  read_segment (segment_path, &segment, &segment_size);
  g_assert_cmpuint (segment_size,
      ==, 64 + strlen (canonical_event_types[0]) + sizeof (payload));

  assert_record_at (segment,
      segment_size, 0, 1, canonical_event_types[0], payload, sizeof (payload));

  remove_tree (root);
}

static void
test_appends_records_in_order (void)
{
  const guint8 first_payload[] = { 0x6f, 0x6e, 0x65 };
  const guint8 second_payload[] = { 0x74, 0x77, 0x6f };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-writer-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;
  const char *event_type = canonical_event_types[0];
  gsize first_len = 0;
  gsize second_len = 0;
  guint64 first_offset = 0;
  guint64 second_offset = 0;
  guint64 first_sequence = 0;
  guint64 second_sequence = 0;
  gsize expected_first_offset = 0;
  gsize expected_second_offset = 0;
  g_autoptr (GBytes) first_bytes = g_bytes_new_static (first_payload,
      sizeof (first_payload));
  g_autoptr (GBytes) second_bytes = g_bytes_new_static (second_payload,
      sizeof (second_payload));

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          first_bytes, &first_offset, &first_sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (first_sequence, ==, 1);

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          second_bytes, &second_offset, &second_sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (second_sequence, ==, 2);

  first_len = strlen (event_type) + sizeof (first_payload) + 64;
  second_len = strlen (event_type) + sizeof (second_payload) + 64;
  expected_second_offset = first_len;

  g_assert_cmpuint (first_offset, ==, expected_first_offset);
  g_assert_cmpuint (second_offset, ==, expected_second_offset);

  read_segment (segment_path, &segment, &segment_size);
  g_assert_cmpuint (segment_size, ==, first_len + second_len);

  assert_record_at (segment,
      segment_size,
      expected_first_offset,
      1, event_type, first_payload, sizeof (first_payload));
  assert_record_at (segment,
      segment_size,
      expected_second_offset,
      2, event_type, second_payload, sizeof (second_payload));

  remove_tree (root);
}

typedef struct
{
  WyreboxJournalWriter *writer;
  GMutex *start_mutex;
  GCond *start_cond;
  gboolean *start;
  guint thread_index;
} ConcurrentAppendContext;

static gpointer
append_concurrent_records (gpointer user_data)
{
  ConcurrentAppendContext *context = user_data;

  g_mutex_lock (context->start_mutex);
  while (!*context->start)
    g_cond_wait (context->start_cond, context->start_mutex);
  g_mutex_unlock (context->start_mutex);

  for (guint index = 0; index < CONCURRENT_APPEND_RECORDS_PER_THREAD; index++) {
    g_autoptr (GError) error = NULL;
    g_autofree gchar *payload = NULL;
    g_autoptr (GBytes) payload_bytes = NULL;
    guint64 offset = 0;
    guint64 sequence = 0;

    payload = g_strdup_printf ("thread=%u record=%u",
        context->thread_index, index);
    payload_bytes = g_bytes_new (payload, strlen (payload));

    g_assert_true (wyrebox_journal_writer_append (context->writer,
            WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED,
            payload_bytes, &offset, &sequence, &error));
    g_assert_no_error (error);
    (void) offset;
    g_assert_cmpuint (sequence, >, 0);
  }

  return NULL;
}

static void
test_concurrent_appends_are_serialized (void)
{
  const guint expected_records =
      CONCURRENT_APPEND_THREADS * CONCURRENT_APPEND_RECORDS_PER_THREAD;
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-writer-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  GThread *threads[CONCURRENT_APPEND_THREADS] = { 0 };
  ConcurrentAppendContext contexts[CONCURRENT_APPEND_THREADS] = { 0 };
  GMutex start_mutex;
  GCond start_cond;
  gboolean start = FALSE;
  guint records_read = 0;
  gboolean eof = FALSE;

  g_assert_nonnull (root);
  g_mutex_init (&start_mutex);
  g_cond_init (&start_cond);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  for (guint index = 0; index < CONCURRENT_APPEND_THREADS; index++) {
    g_autofree gchar *thread_name = NULL;

    contexts[index].writer = writer;
    contexts[index].start_mutex = &start_mutex;
    contexts[index].start_cond = &start_cond;
    contexts[index].start = &start;
    contexts[index].thread_index = index;
    thread_name = g_strdup_printf ("journal-append-%u", index);
    threads[index] = g_thread_new (thread_name, append_concurrent_records,
        &contexts[index]);
  }

  g_mutex_lock (&start_mutex);
  start = TRUE;
  g_cond_broadcast (&start_cond);
  g_mutex_unlock (&start_mutex);

  for (guint index = 0; index < CONCURRENT_APPEND_THREADS; index++)
    g_thread_join (threads[index]);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  while (TRUE) {
    g_auto (WyreboxJournalRecord) record = { 0 };
    gsize payload_size = 0;

    if (!wyrebox_journal_reader_read_next (reader, &record, &eof, &error)) {
      g_assert_no_error (error);
      g_assert_true (eof);
      break;
    }

    records_read++;
    g_assert_cmpuint (record.sequence, ==, records_read);
    g_assert_cmpint (record.event_type, ==,
        WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED);
    g_assert_nonnull (g_bytes_get_data (record.payload, &payload_size));
    g_assert_cmpuint (payload_size, >, 0);
  }

  g_assert_cmpuint (records_read, ==, expected_records);

  g_cond_clear (&start_cond);
  g_mutex_clear (&start_mutex);
  remove_tree (root);
}

static void
test_supports_all_canonical_event_names (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-writer-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;
  gsize offset = 0;
  gsize expected_size = 0;
  guint64 out_offset = 0;
  guint64 out_sequence = 0;
  g_autoptr (GBytes) empty_payload = g_bytes_new_static ("", 0);

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);

  for (guint index = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
      index <= WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED; index++) {
    const gchar *event_type = wyrebox_journal_event_type_to_string (index);

    g_assert_nonnull (event_type);
    g_assert_cmpstr (event_type, ==, canonical_event_types[index]);

    g_assert_true (wyrebox_journal_writer_append (writer,
            index, empty_payload, &out_offset, &out_sequence, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (out_sequence, ==, (guint64) (index + 1));
    g_assert_cmpuint (out_offset, ==, offset);

    offset += 64 + strlen (event_type);
  }

  read_segment (segment_path, &segment, &segment_size);
  g_assert_cmpuint (segment_size, ==, offset);

  for (guint index = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
      index <= WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED; index++) {
    const gchar *event_type = wyrebox_journal_event_type_to_string (index);

    assert_record_at (segment,
        segment_size, expected_size, index + 1, event_type, NULL, 0);
    expected_size += 64 + strlen (event_type);
  }

  remove_tree (root);
}

static void
test_rejects_invalid_event_type (void)
{
  const guint8 payload[] = { 0x62, 0x61, 0x64 };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-writer-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  GStatBuf before = { 0 };
  GStatBuf after = { 0 };
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));
  guint64 out_offset = 0;
  guint64 out_sequence = 0;

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);

  g_assert_cmpint (g_stat (segment_path, &before), ==, 0);
  g_assert_false (wyrebox_journal_writer_append (writer,
          (WyreboxJournalEventType) 100,
          payload_bytes, &out_offset, &out_sequence, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_cmpint (g_stat (segment_path, &after), ==, 0);
  g_assert_cmpint (before.st_size, ==, after.st_size);

  remove_tree (root);
}

static void
test_rejects_existing_nonempty_segment (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-journal-writer-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;

  g_assert_nonnull (root);
  segment_path = segment_path_for_root (root);

  g_assert_true (g_file_set_contents (segment_path, "stub", -1, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_null (writer);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/journal-writer/new-creates-empty-segment",
      test_new_creates_empty_segment);
  g_test_add_func ("/journal-writer/appends-message-delivered-record",
      test_appends_message_delivered_record);
  g_test_add_func ("/journal-writer/appends-records-in-order",
      test_appends_records_in_order);
  g_test_add_func ("/journal-writer/concurrent-appends-are-serialized",
      test_concurrent_appends_are_serialized);
  g_test_add_func ("/journal-writer/supports-all-canonical-event-names",
      test_supports_all_canonical_event_names);
  g_test_add_func ("/journal-writer/rejects-invalid-event-type",
      test_rejects_invalid_event_type);
  g_test_add_func ("/journal-writer/rejects-existing-nonempty-segment",
      test_rejects_existing_nonempty_segment);

  return g_test_run ();
}
