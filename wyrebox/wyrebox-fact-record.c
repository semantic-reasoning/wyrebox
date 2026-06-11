#include "wyrebox-fact-record.h"

#include <string.h>

#include <gio/gio.h>

#define WYREBOX_FACT_CONFIDENCE_EXACT 1000000u

void
wyrebox_fact_record_clear (WyreboxFactRecord *record)
{
  if (record == NULL)
    return;

  g_clear_pointer (&record->predicate, g_free);
  g_clear_pointer (&record->args, g_strfreev);
  g_clear_pointer (&record->source, g_free);
  record->confidence_ppm = 0;
  record->created_at_unix_us = 0;
  record->retracted_at_unix_us = 0;
}

static gboolean
is_predicate_start (char value)
{
  return g_ascii_islower (value) || value == '_';
}

static gboolean
is_predicate_char (char value)
{
  return g_ascii_isalnum (value) || value == '_';
}

static gboolean
validate_predicate (const char *predicate, GError **error)
{
  if (predicate == NULL || *predicate == '\0') {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "fact predicate is required");
    return FALSE;
  }

  if (!is_predicate_start (predicate[0])) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact predicate must start with lowercase ASCII or underscore");
    return FALSE;
  }

  for (const char *cursor = predicate + 1; *cursor != '\0'; cursor++) {
    if (!is_predicate_char (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "fact predicate contains an unsupported character");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_args (const char *const *args, GError **error)
{
  if (args == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact args vector is required");
    return FALSE;
  }

  for (guint index = 0; args[index] != NULL; index++) {
    if (args[index][0] == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "fact args must not be empty");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_source (const char *source, GError **error)
{
  if (source == NULL || *source == '\0') {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "fact source is required");
    return FALSE;
  }

  return TRUE;
}

gboolean
wyrebox_fact_record_init (WyreboxFactRecord *record,
    const char *predicate,
    const char *const *args,
    const char *source,
    guint32 confidence_ppm, guint64 created_at_unix_us, GError **error)
{
  g_auto (WyreboxFactRecord) next = { 0 };

  g_return_val_if_fail (record != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_predicate (predicate, error))
    return FALSE;

  if (!validate_args (args, error))
    return FALSE;

  if (!validate_source (source, error))
    return FALSE;

  if (confidence_ppm > WYREBOX_FACT_CONFIDENCE_EXACT) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact confidence must be <= %" G_GUINT32_FORMAT,
        WYREBOX_FACT_CONFIDENCE_EXACT);
    return FALSE;
  }

  if (created_at_unix_us == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact created timestamp is required");
    return FALSE;
  }

  next.predicate = g_strdup (predicate);
  next.args = g_strdupv ((char **) args);
  next.source = g_strdup (source);
  next.confidence_ppm = confidence_ppm;
  next.created_at_unix_us = created_at_unix_us;

  wyrebox_fact_record_clear (record);
  *record = next;
  memset (&next, 0, sizeof (next));

  return TRUE;
}

gboolean
wyrebox_fact_record_mark_retracted (WyreboxFactRecord *record,
    guint64 retracted_at_unix_us, GError **error)
{
  g_return_val_if_fail (record != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (record->created_at_unix_us == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "cannot retract an uninitialized fact");
    return FALSE;
  }

  if (retracted_at_unix_us < record->created_at_unix_us) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact retraction timestamp must not precede creation");
    return FALSE;
  }

  record->retracted_at_unix_us = retracted_at_unix_us;
  return TRUE;
}

static void
append_wirelog_escaped_string (GString *builder, const char *value)
{
  g_string_append_c (builder, '"');

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    switch (*cursor) {
      case '\\':
        g_string_append (builder, "\\\\");
        break;
      case '"':
        g_string_append (builder, "\\\"");
        break;
      case '\n':
        g_string_append (builder, "\\n");
        break;
      case '\r':
        g_string_append (builder, "\\r");
        break;
      case '\t':
        g_string_append (builder, "\\t");
        break;
      default:
        g_string_append_c (builder, *cursor);
        break;
    }
  }

  g_string_append_c (builder, '"');
}

char *
wyrebox_fact_record_to_wirelog_fact (const WyreboxFactRecord *record,
    GError **error)
{
  g_autoptr (GString) builder = NULL;

  g_return_val_if_fail (record != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!validate_predicate (record->predicate, error))
    return NULL;

  if (!validate_args ((const char *const *) record->args, error))
    return NULL;

  builder = g_string_new (record->predicate);
  g_string_append_c (builder, '(');

  for (guint index = 0; record->args[index] != NULL; index++) {
    if (index > 0)
      g_string_append (builder, ", ");

    append_wirelog_escaped_string (builder, record->args[index]);
  }

  g_string_append (builder, ").");
  return g_string_free (g_steal_pointer (&builder), FALSE);
}

char *
wyrebox_fact_record_array_to_wirelog_facts (GPtrArray *records, GError **error)
{
  g_autoptr (GString) builder = NULL;

  g_return_val_if_fail (records != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  builder = g_string_new (NULL);

  for (guint index = 0; index < records->len; index++) {
    const WyreboxFactRecord *record = g_ptr_array_index (records, index);
    g_autofree char *line = NULL;

    if (record == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "fact record at index %u is required", index);
      return NULL;
    }

    line = wyrebox_fact_record_to_wirelog_fact (record, error);
    if (line == NULL)
      return NULL;

    g_string_append (builder, line);
    g_string_append_c (builder, '\n');
  }

  return g_string_free (g_steal_pointer (&builder), FALSE);
}

gboolean
wyrebox_fact_record_array_write_wirelog_facts (GPtrArray *records,
    GOutputStream *stream, GCancellable *cancellable, GError **error)
{
  g_autofree char *text = NULL;
  gsize bytes_written = 0;

  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  text = wyrebox_fact_record_array_to_wirelog_facts (records, error);
  if (text == NULL)
    return FALSE;

  return g_output_stream_write_all (stream,
      text, strlen (text), &bytes_written, cancellable, error);
}

static gboolean
write_wirelog_text_and_close (GOutputStream *stream,
    const char *text, GCancellable *cancellable, GError **error)
{
  g_autoptr (GError) close_error = NULL;
  gsize bytes_written = 0;

  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_output_stream_write_all (stream,
          text, strlen (text), &bytes_written, cancellable, error)) {
    (void) g_output_stream_close (stream, cancellable, &close_error);
    return FALSE;
  }

  return g_output_stream_close (stream, cancellable, error);
}

gboolean
wyrebox_fact_record_array_write_wirelog_facts_and_close (GPtrArray *records,
    GOutputStream *stream, GCancellable *cancellable, GError **error)
{
  g_autofree char *text = NULL;

  g_return_val_if_fail (stream != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  text = wyrebox_fact_record_array_to_wirelog_facts (records, error);
  if (text == NULL)
    return FALSE;

  return write_wirelog_text_and_close (stream, text, cancellable, error);
}

gboolean
wyrebox_fact_record_array_write_wirelog_fact_file (GPtrArray *records,
    GFile *file, GCancellable *cancellable, GError **error)
{
  g_autoptr (GFileOutputStream) stream = NULL;
  g_autofree char *text = NULL;

  g_return_val_if_fail (file != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  text = wyrebox_fact_record_array_to_wirelog_facts (records, error);
  if (text == NULL)
    return FALSE;

  stream = g_file_replace (file,
      NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, cancellable, error);
  if (stream == NULL)
    return FALSE;

  return write_wirelog_text_and_close (G_OUTPUT_STREAM (stream),
      text, cancellable, error);
}

static gboolean
is_dump_filename_char (char value)
{
  return g_ascii_isalnum (value) || value == '_' || value == '-';
}

char *
wyrebox_fact_record_build_wirelog_dump_filename (const char *batch_id,
    guint64 journal_sequence, GError **error)
{
  g_autoptr (GString) sanitized = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (batch_id == NULL || batch_id[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Wirelog dump batch id is required");
    return NULL;
  }

  sanitized = g_string_new (NULL);
  for (const char *cursor = batch_id; *cursor != '\0'; cursor++) {
    if (is_dump_filename_char (*cursor))
      g_string_append_c (sanitized, *cursor);
    else
      g_string_append_c (sanitized, '_');
  }

  return g_strdup_printf ("%020" G_GUINT64_FORMAT "-%s.wl",
      journal_sequence, sanitized->str);
}

GFile *
wyrebox_fact_record_array_write_wirelog_dump (GPtrArray *records,
    GFile *directory,
    const char *batch_id,
    guint64 journal_sequence, GCancellable *cancellable, GError **error)
{
  g_autofree char *filename = NULL;
  g_autoptr (GFile) output = NULL;

  g_return_val_if_fail (directory != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  filename = wyrebox_fact_record_build_wirelog_dump_filename (batch_id,
      journal_sequence, error);
  if (filename == NULL)
    return NULL;

  output = g_file_get_child (directory, filename);
  if (!wyrebox_fact_record_array_write_wirelog_fact_file (records,
          output, cancellable, error))
    return NULL;

  return g_steal_pointer (&output);
}
