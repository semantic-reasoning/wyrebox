#include "wyrebox-eml-metadata.h"

#include <string.h>

#include <gio/gio.h>

void
wyrebox_eml_metadata_clear (WyreboxEmlMetadata *metadata)
{
  if (metadata == NULL)
    return;

  g_clear_pointer (&metadata->message_id, g_free);
  g_clear_pointer (&metadata->subject, g_free);
  g_clear_pointer (&metadata->from, g_free);
  g_clear_pointer (&metadata->to, g_free);
  g_clear_pointer (&metadata->cc, g_free);
  g_clear_pointer (&metadata->bcc, g_free);
  g_clear_pointer (&metadata->date, g_free);
  g_clear_pointer (&metadata->in_reply_to, g_free);
  g_clear_pointer (&metadata->references, g_free);
  metadata->size_bytes = 0;
  metadata->duplicate_message_id_count = 0;
}

static const guint8 *
find_header_separator (const guint8 *data, gsize size)
{
  if (size < 4)
    return NULL;

  for (gsize index = 0; index <= size - 4; index++) {
    if (data[index] == '\r' &&
        data[index + 1] == '\n' &&
        data[index + 2] == '\r' && data[index + 3] == '\n')
      return data + index;
  }

  return NULL;
}

static void
set_first_value (char **slot, GString *value)
{
  if (*slot == NULL)
    *slot = g_strdup (value->str);
}

static void
clear_g_string (GString **string)
{
  if (*string == NULL)
    return;

  g_string_free (*string, TRUE);
  *string = NULL;
}

static void
commit_header (WyreboxEmlMetadata *metadata, const char *name, GString *value)
{
  if (name == NULL || value == NULL)
    return;

  if (g_ascii_strcasecmp (name, "Message-ID") == 0) {
    if (metadata->message_id == NULL)
      metadata->message_id = g_strdup (value->str);
    else
      metadata->duplicate_message_id_count++;
  } else if (g_ascii_strcasecmp (name, "Subject") == 0) {
    set_first_value (&metadata->subject, value);
  } else if (g_ascii_strcasecmp (name, "From") == 0) {
    set_first_value (&metadata->from, value);
  } else if (g_ascii_strcasecmp (name, "To") == 0) {
    set_first_value (&metadata->to, value);
  } else if (g_ascii_strcasecmp (name, "Cc") == 0) {
    set_first_value (&metadata->cc, value);
  } else if (g_ascii_strcasecmp (name, "Bcc") == 0) {
    set_first_value (&metadata->bcc, value);
  } else if (g_ascii_strcasecmp (name, "Date") == 0) {
    set_first_value (&metadata->date, value);
  } else if (g_ascii_strcasecmp (name, "In-Reply-To") == 0) {
    set_first_value (&metadata->in_reply_to, value);
  } else if (g_ascii_strcasecmp (name, "References") == 0) {
    set_first_value (&metadata->references, value);
  }
}

static void
replace_current_header (char **current_name,
    GString **current_value,
    const guint8 *line, gsize line_len, const guint8 *colon)
{
  const guint8 *value = colon + 1;
  gsize name_len = (gsize) (colon - line);
  gsize value_len = line_len - name_len - 1;

  while (value_len > 0 && (*value == ' ' || *value == '\t')) {
    value++;
    value_len--;
  }

  g_clear_pointer (current_name, g_free);
  clear_g_string (current_value);

  *current_name = g_strndup ((const char *) line, name_len);
  *current_value = g_string_new_len ((const char *) value, value_len);
}

static void
append_continuation (GString *current_value, const guint8 *line, gsize line_len)
{
  const guint8 *value = line;
  gsize value_len = line_len;

  while (value_len > 0 && (*value == ' ' || *value == '\t')) {
    value++;
    value_len--;
  }

  g_string_append_c (current_value, ' ');
  g_string_append_len (current_value, (const char *) value, value_len);
}

gboolean
wyrebox_eml_metadata_parse_bytes (GBytes *bytes,
    WyreboxEmlMetadata *out_metadata, GError **error)
{
  g_auto (WyreboxEmlMetadata) metadata = { 0 };
  g_autofree char *current_name = NULL;
  g_autoptr (GString) current_value = NULL;
  const guint8 *data = NULL;
  const guint8 *separator = NULL;
  gsize size = 0;
  gsize header_len = 0;
  gsize offset = 0;

  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_metadata != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  data = g_bytes_get_data (bytes, &size);
  separator = find_header_separator (data, size);
  if (separator == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "missing RFC 5322 header/body separator");
    return FALSE;
  }

  metadata.size_bytes = (guint64) size;
  header_len = (gsize) (separator - data);

  while (offset < header_len) {
    gsize line_start = offset;
    gsize line_end = offset;
    gsize line_len = 0;
    gboolean has_line_terminator = FALSE;
    const guint8 *line = NULL;
    const guint8 *colon = NULL;

    while (line_end + 1 < header_len) {
      if (data[line_end] == '\r' && data[line_end + 1] == '\n') {
        has_line_terminator = TRUE;
        break;
      }

      line_end++;
    }

    line = data + line_start;
    if (has_line_terminator)
      line_len = line_end - line_start;
    else
      line_len = header_len - line_start;

    if (line_len > 0 &&
        (line[0] == ' ' || line[0] == '\t') && current_value != NULL) {
      append_continuation (current_value, line, line_len);
    } else {
      commit_header (&metadata, current_name, current_value);
      g_clear_pointer (&current_name, g_free);
      clear_g_string (&current_value);

      colon = memchr (line, ':', line_len);
      if (colon != NULL)
        replace_current_header (&current_name, &current_value,
            line, line_len, colon);
    }

    if (has_line_terminator)
      offset = line_end + 2;
    else
      offset = header_len;
  }

  commit_header (&metadata, current_name, current_value);

  *out_metadata = metadata;
  metadata.message_id = NULL;
  metadata.subject = NULL;
  metadata.from = NULL;
  metadata.to = NULL;
  metadata.cc = NULL;
  metadata.bcc = NULL;
  metadata.date = NULL;
  metadata.in_reply_to = NULL;
  metadata.references = NULL;
  metadata.size_bytes = 0;
  metadata.duplicate_message_id_count = 0;

  return TRUE;
}
