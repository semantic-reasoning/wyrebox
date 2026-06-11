#include "wyrebox-fact-record.h"

#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *name = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, name, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

typedef struct
{
  GOutputStream parent_instance;
  gboolean fail_write;
  gboolean fail_close;
  guint close_calls;
} TestOutputStream;

typedef struct
{
  GOutputStreamClass parent_class;
} TestOutputStreamClass;

G_DEFINE_TYPE (TestOutputStream, test_output_stream, G_TYPE_OUTPUT_STREAM)
     static gssize
         test_output_stream_write_fn (GOutputStream *stream,
    const void *buffer, gsize count, GCancellable *cancellable, GError **error)
{
  TestOutputStream *self = (TestOutputStream *) stream;

  (void) buffer;
  (void) count;
  (void) cancellable;

  if (self->fail_write) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "forced write failure");
    return -1;
  }

  return (gssize) count;
}

static gboolean
test_output_stream_close_fn (GOutputStream *stream,
    GCancellable *cancellable, GError **error)
{
  TestOutputStream *self = (TestOutputStream *) stream;

  (void) cancellable;

  self->close_calls++;
  if (self->fail_close) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "forced close failure");
    return FALSE;
  }

  return TRUE;
}

static void
test_output_stream_class_init (TestOutputStreamClass *klass)
{
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);

  stream_class->write_fn = test_output_stream_write_fn;
  stream_class->close_fn = test_output_stream_close_fn;
}

static void
test_output_stream_init (TestOutputStream *self)
{
  (void) self;
}

static GOutputStream *
test_output_stream_new (gboolean fail_write, gboolean fail_close)
{
  TestOutputStream *stream = NULL;

  stream = g_object_new (test_output_stream_get_type (), NULL);
  stream->fail_write = fail_write;
  stream->fail_close = fail_close;

  return G_OUTPUT_STREAM (stream);
}

static void
test_fact_record_copies_owned_fields (void)
{
  const char *args[] = {
    "mail-1",
    "example.test",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_true (wyrebox_fact_record_init (&record,
          "sender_domain",
          args, "header:from", 1000000, 1800000000000000, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (record.predicate, ==, "sender_domain");
  g_assert_nonnull (record.args);
  g_assert_cmpstr (record.args[0], ==, "mail-1");
  g_assert_cmpstr (record.args[1], ==, "example.test");
  g_assert_null (record.args[2]);
  g_assert_cmpstr (record.source, ==, "header:from");
  g_assert_cmpuint (record.confidence_ppm, ==, 1000000);
  g_assert_cmpuint (record.created_at_unix_us, ==, 1800000000000000);
  g_assert_cmpuint (record.retracted_at_unix_us, ==, 0);
}

static void
test_fact_record_rejects_invalid_predicate (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_false (wyrebox_fact_record_init (&record,
          "SenderDomain", args, "header:from", 1000000,
          1800000000000000, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (record.predicate);
  g_assert_null (record.args);
  g_assert_null (record.source);
}

static void
test_fact_record_rejects_missing_provenance (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_false (wyrebox_fact_record_init (&record,
          "participant", args, "", 1000000, 1800000000000000, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_record_marks_retraction_after_creation (void)
{
  const char *args[] = {
    "mail-1",
    "parent-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_true (wyrebox_fact_record_init (&record,
          "references", args, "header:references", 1000000,
          1800000000000000, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_fact_record_mark_retracted (&record,
          1800000000000001, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (record.retracted_at_unix_us, ==, 1800000000000001);
}

static void
test_fact_record_rejects_retraction_before_creation (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_true (wyrebox_fact_record_init (&record,
          "participant", args, "header:to", 1000000, 1800000000000000, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_fact_record_mark_retracted (&record,
          1799999999999999, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_record_serializes_wirelog_fact (void)
{
  const char *args[] = {
    "mail-1",
    "example.test",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };
  g_autofree char *text = NULL;

  g_assert_true (wyrebox_fact_record_init (&record,
          "sender_domain",
          args, "header:from", 1000000, 1800000000000000, &error));
  g_assert_no_error (error);

  text = wyrebox_fact_record_to_wirelog_fact (&record, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (text, ==, "sender_domain(\"mail-1\", \"example.test\").");
}

static void
test_fact_record_serializes_escaped_wirelog_args (void)
{
  const char *args[] = {
    "mail\"1",
    "line\\one\nline\ttwo",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };
  g_autofree char *text = NULL;

  g_assert_true (wyrebox_fact_record_init (&record,
          "participant", args, "header:to", 1000000, 1800000000000000, &error));
  g_assert_no_error (error);

  text = wyrebox_fact_record_to_wirelog_fact (&record, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (text, ==,
      "participant(\"mail\\\"1\", \"line\\\\one\\nline\\ttwo\").");
}

static void
test_fact_record_rejects_uninitialized_wirelog_serialization (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };
  g_autofree char *text = NULL;

  text = wyrebox_fact_record_to_wirelog_fact (&record, &error);
  g_assert_null (text);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static WyreboxFactRecord *
test_fact_record_new (const char *predicate,
    const char *const *args, const char *source)
{
  g_autoptr (GError) error = NULL;
  WyreboxFactRecord *record = NULL;

  record = g_new0 (WyreboxFactRecord, 1);
  g_assert_true (wyrebox_fact_record_init (record,
          predicate, args, source, 1000000, 1800000000000000, &error));
  g_assert_no_error (error);

  return record;
}

static void
test_fact_record_free (gpointer data)
{
  WyreboxFactRecord *record = data;

  wyrebox_fact_record_clear (record);
  g_free (record);
}

static void
test_fact_record_serializes_empty_wirelog_batch (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autofree char *text = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  text = wyrebox_fact_record_array_to_wirelog_facts (records, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (text, ==, "");
}

static void
test_fact_record_serializes_wirelog_batch (void)
{
  const char *sender_args[] = {
    "mail-1",
    "example.test",
    NULL,
  };
  const char *participant_args[] = {
    "mail-1",
    "Alice <alice@example.test>",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autofree char *text = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("sender_domain", sender_args, "header:from"));
  g_ptr_array_add (records,
      test_fact_record_new ("participant", participant_args, "header:from"));

  text = wyrebox_fact_record_array_to_wirelog_facts (records, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (text, ==,
      "sender_domain(\"mail-1\", \"example.test\").\n"
      "participant(\"mail-1\", \"Alice <alice@example.test>\").\n");
}

static void
test_fact_record_wirelog_batch_propagates_invalid_record (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autofree char *text = NULL;
  WyreboxFactRecord *record = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  record = test_fact_record_new ("participant", args, "header:to");
  g_clear_pointer (&record->predicate, g_free);
  g_ptr_array_add (records, record);

  text = wyrebox_fact_record_array_to_wirelog_facts (records, &error);
  g_assert_null (text);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_record_wirelog_batch_rejects_null_record (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autofree char *text = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("participant", args, "header:to"));
  g_ptr_array_add (records, NULL);

  text = wyrebox_fact_record_array_to_wirelog_facts (records, &error);
  g_assert_null (text);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_true (g_strrstr (error->message, "index 1") != NULL);
}

static void
test_fact_record_writes_wirelog_batch_to_stream (void)
{
  const char *sender_args[] = {
    "mail-1",
    "example.test",
    NULL,
  };
  const char *participant_args[] = {
    "mail-1",
    "Alice <alice@example.test>",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GOutputStream) stream = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("sender_domain", sender_args, "header:from"));
  g_ptr_array_add (records,
      test_fact_record_new ("participant", participant_args, "header:from"));
  stream = g_memory_output_stream_new_resizable ();

  g_assert_true (wyrebox_fact_record_array_write_wirelog_facts (records,
          stream, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_output_stream_close (stream, NULL, &error));
  g_assert_no_error (error);

  data = g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (stream));
  size = g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (stream));
  g_assert_cmpmem (data, size,
      "sender_domain(\"mail-1\", \"example.test\").\n"
      "participant(\"mail-1\", \"Alice <alice@example.test>\").\n",
      strlen ("sender_domain(\"mail-1\", \"example.test\").\n"
          "participant(\"mail-1\", \"Alice <alice@example.test>\").\n"));
}

static void
test_fact_record_writes_empty_wirelog_batch_to_stream (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GOutputStream) stream = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  stream = g_memory_output_stream_new_resizable ();

  g_assert_true (wyrebox_fact_record_array_write_wirelog_facts (records,
          stream, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_output_stream_close (stream, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM
          (stream)), ==, 0);
}

static void
test_fact_record_wirelog_batch_write_propagates_stream_failure (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GOutputStream) stream = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("participant", args, "header:to"));
  stream = g_memory_output_stream_new_resizable ();
  g_assert_true (g_output_stream_close (stream, NULL, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_fact_record_array_write_wirelog_facts (records,
          stream, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED);
}

static void
test_fact_record_wirelog_batch_write_and_close_preserves_write_error (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GOutputStream) stream = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("participant", args, "header:to"));
  stream = test_output_stream_new (TRUE, TRUE);

  g_assert_false (wyrebox_fact_record_array_write_wirelog_facts_and_close
      (records, stream, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "forced write failure");
  g_assert_cmpuint (((TestOutputStream *) stream)->close_calls, ==, 1);
}

static void
test_fact_record_wirelog_batch_write_and_close_propagates_close_error (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GOutputStream) stream = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("participant", args, "header:to"));
  stream = test_output_stream_new (FALSE, TRUE);

  g_assert_false (wyrebox_fact_record_array_write_wirelog_facts_and_close
      (records, stream, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "forced close failure");
  g_assert_cmpuint (((TestOutputStream *) stream)->close_calls, ==, 1);
}

static void
    test_fact_record_wirelog_batch_write_and_close_keeps_stream_on_validation_error
    (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GOutputStream) stream = NULL;
  WyreboxFactRecord *record = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  record = test_fact_record_new ("participant", args, "header:to");
  g_clear_pointer (&record->predicate, g_free);
  g_ptr_array_add (records, record);
  stream = test_output_stream_new (FALSE, FALSE);

  g_assert_false (wyrebox_fact_record_array_write_wirelog_facts_and_close
      (records, stream, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (((TestOutputStream *) stream)->close_calls, ==, 0);
}

static void
test_fact_record_writes_wirelog_batch_to_file (void)
{
  const char *args[] = {
    "mail-1",
    "example.test",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autofree char *root = g_dir_make_tmp ("wyrebox-fact-record-XXXXXX", NULL);
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;

  g_assert_nonnull (root);
  path = g_build_filename (root, "facts.wl", NULL);
  file = g_file_new_for_path (path);
  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("sender_domain", args, "header:from"));

  g_assert_true (wyrebox_fact_record_array_write_wirelog_fact_file (records,
          file, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_get_contents (path, &contents, &length, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (contents, length,
      "sender_domain(\"mail-1\", \"example.test\").\n",
      strlen ("sender_domain(\"mail-1\", \"example.test\").\n"));

  remove_tree (root);
}

static void
test_fact_record_wirelog_file_writer_replaces_existing_file (void)
{
  const char *args[] = {
    "mail-1",
    "example.test",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autofree char *root = g_dir_make_tmp ("wyrebox-fact-record-XXXXXX", NULL);
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;

  g_assert_nonnull (root);
  path = g_build_filename (root, "facts.wl", NULL);
  g_assert_true (g_file_set_contents (path, "stale\n", -1, &error));
  g_assert_no_error (error);
  file = g_file_new_for_path (path);
  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("sender_domain", args, "header:from"));

  g_assert_true (wyrebox_fact_record_array_write_wirelog_fact_file (records,
          file, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_get_contents (path, &contents, &length, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (contents, length,
      "sender_domain(\"mail-1\", \"example.test\").\n",
      strlen ("sender_domain(\"mail-1\", \"example.test\").\n"));

  remove_tree (root);
}

static void
test_fact_record_wirelog_file_writer_propagates_replace_failure (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autoptr (GFile) file = NULL;

  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  file = g_file_new_for_path ("/no/such/wyrebox/facts.wl");

  g_assert_false (wyrebox_fact_record_array_write_wirelog_fact_file (records,
          file, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
test_fact_record_wirelog_file_writer_serializes_before_replace (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autofree char *root = g_dir_make_tmp ("wyrebox-fact-record-XXXXXX", NULL);
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;
  WyreboxFactRecord *record = NULL;

  g_assert_nonnull (root);
  path = g_build_filename (root, "facts.wl", NULL);
  g_assert_true (g_file_set_contents (path, "stale\n", -1, &error));
  g_assert_no_error (error);
  file = g_file_new_for_path (path);
  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  record = test_fact_record_new ("participant", args, "header:to");
  g_clear_pointer (&record->predicate, g_free);
  g_ptr_array_add (records, record);

  g_assert_false (wyrebox_fact_record_array_write_wirelog_fact_file (records,
          file, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_true (g_file_get_contents (path, &contents, &length, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (contents, length, "stale\n", strlen ("stale\n"));

  remove_tree (root);
}

static void
test_fact_record_wirelog_file_readback_preserves_batch_format (void)
{
  const char *participant_args[] = {
    "mail\"1",
    "line\\one\nline\ttwo",
    NULL,
  };
  const char *sender_args[] = {
    "mail-1",
    "example.test",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) records = NULL;
  g_autofree char *root = g_dir_make_tmp ("wyrebox-fact-record-XXXXXX", NULL);
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;
  const char *expected =
      "participant(\"mail\\\"1\", \"line\\\\one\\nline\\ttwo\").\n"
      "sender_domain(\"mail-1\", \"example.test\").\n";

  g_assert_nonnull (root);
  path = g_build_filename (root, "facts.wl", NULL);
  file = g_file_new_for_path (path);
  records = g_ptr_array_new_with_free_func (test_fact_record_free);
  g_ptr_array_add (records,
      test_fact_record_new ("participant", participant_args, "header:to"));
  g_ptr_array_add (records,
      test_fact_record_new ("sender_domain", sender_args, "header:from"));

  g_assert_true (wyrebox_fact_record_array_write_wirelog_fact_file (records,
          file, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_get_contents (path, &contents, &length, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (contents, length, expected, strlen (expected));

  remove_tree (root);
}

static void
test_fact_record_builds_wirelog_dump_filename (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *filename = NULL;

  filename =
      wyrebox_fact_record_build_wirelog_dump_filename ("mail-1", 42, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (filename, ==, "00000000000000000042-mail-1.wl");
}

static void
test_fact_record_wirelog_dump_filename_sanitizes_identifier (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *filename = NULL;

  filename =
      wyrebox_fact_record_build_wirelog_dump_filename ("../mail id/one", 7,
      &error);
  g_assert_no_error (error);
  g_assert_cmpstr (filename, ==, "00000000000000000007-___mail_id_one.wl");
}

static void
test_fact_record_wirelog_dump_filename_rejects_missing_identifier (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *filename = NULL;

  filename = wyrebox_fact_record_build_wirelog_dump_filename ("", 7, &error);
  g_assert_null (filename);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_record_wirelog_dump_filename_rejects_null_identifier (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *filename = NULL;

  filename = wyrebox_fact_record_build_wirelog_dump_filename (NULL, 7, &error);
  g_assert_null (filename);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_record_wirelog_dump_filename_allows_zero_sequence (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *filename = NULL;

  filename =
      wyrebox_fact_record_build_wirelog_dump_filename ("bootstrap", 0, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (filename, ==, "00000000000000000000-bootstrap.wl");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/facts/fact-record/copies-owned-fields",
      test_fact_record_copies_owned_fields);
  g_test_add_func ("/facts/fact-record/rejects-invalid-predicate",
      test_fact_record_rejects_invalid_predicate);
  g_test_add_func ("/facts/fact-record/rejects-missing-provenance",
      test_fact_record_rejects_missing_provenance);
  g_test_add_func ("/facts/fact-record/marks-retraction-after-creation",
      test_fact_record_marks_retraction_after_creation);
  g_test_add_func ("/facts/fact-record/rejects-retraction-before-creation",
      test_fact_record_rejects_retraction_before_creation);
  g_test_add_func ("/facts/fact-record/serializes-wirelog-fact",
      test_fact_record_serializes_wirelog_fact);
  g_test_add_func ("/facts/fact-record/serializes-escaped-wirelog-args",
      test_fact_record_serializes_escaped_wirelog_args);
  g_test_add_func ("/facts/fact-record/"
      "rejects-uninitialized-wirelog-serialization",
      test_fact_record_rejects_uninitialized_wirelog_serialization);
  g_test_add_func ("/facts/fact-record/serializes-empty-wirelog-batch",
      test_fact_record_serializes_empty_wirelog_batch);
  g_test_add_func ("/facts/fact-record/serializes-wirelog-batch",
      test_fact_record_serializes_wirelog_batch);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-batch-propagates-invalid-record",
      test_fact_record_wirelog_batch_propagates_invalid_record);
  g_test_add_func ("/facts/fact-record/wirelog-batch-rejects-null-record",
      test_fact_record_wirelog_batch_rejects_null_record);
  g_test_add_func ("/facts/fact-record/writes-wirelog-batch-to-stream",
      test_fact_record_writes_wirelog_batch_to_stream);
  g_test_add_func ("/facts/fact-record/writes-empty-wirelog-batch-to-stream",
      test_fact_record_writes_empty_wirelog_batch_to_stream);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-batch-write-propagates-stream-failure",
      test_fact_record_wirelog_batch_write_propagates_stream_failure);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-batch-write-and-close-preserves-write-error",
      test_fact_record_wirelog_batch_write_and_close_preserves_write_error);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-batch-write-and-close-propagates-close-error",
      test_fact_record_wirelog_batch_write_and_close_propagates_close_error);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-batch-write-and-close-keeps-stream-on-validation-error",
      test_fact_record_wirelog_batch_write_and_close_keeps_stream_on_validation_error);
  g_test_add_func ("/facts/fact-record/writes-wirelog-batch-to-file",
      test_fact_record_writes_wirelog_batch_to_file);
  g_test_add_func ("/facts/fact-record/wirelog-file-writer-replaces-existing",
      test_fact_record_wirelog_file_writer_replaces_existing_file);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-file-writer-propagates-replace-failure",
      test_fact_record_wirelog_file_writer_propagates_replace_failure);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-file-writer-serializes-before-replace",
      test_fact_record_wirelog_file_writer_serializes_before_replace);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-file-readback-preserves-batch-format",
      test_fact_record_wirelog_file_readback_preserves_batch_format);
  g_test_add_func ("/facts/fact-record/builds-wirelog-dump-filename",
      test_fact_record_builds_wirelog_dump_filename);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-dump-filename-sanitizes-identifier",
      test_fact_record_wirelog_dump_filename_sanitizes_identifier);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-dump-filename-rejects-missing-identifier",
      test_fact_record_wirelog_dump_filename_rejects_missing_identifier);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-dump-filename-rejects-null-identifier",
      test_fact_record_wirelog_dump_filename_rejects_null_identifier);
  g_test_add_func ("/facts/fact-record/"
      "wirelog-dump-filename-allows-zero-sequence",
      test_fact_record_wirelog_dump_filename_allows_zero_sequence);

  return g_test_run ();
}
