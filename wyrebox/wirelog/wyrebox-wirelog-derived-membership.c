#include "wyrebox-wirelog-derived-membership.h"

#include <gio/gio.h>

static void
membership_ptr_free (gpointer data)
{
  wyrebox_wirelog_derived_membership_free (data);
}

static GPtrArray *
membership_array_new (void)
{
  return g_ptr_array_new_with_free_func (membership_ptr_free);
}

void
wyrebox_wirelog_derived_membership_clear (WyreboxWirelogDerivedMembership
    *membership)
{
  if (membership == NULL)
    return;

  g_clear_pointer (&membership->view_id, g_free);
  g_clear_pointer (&membership->message_id, g_free);
}

void
wyrebox_wirelog_derived_membership_free (WyreboxWirelogDerivedMembership
    *membership)
{
  if (membership == NULL)
    return;

  wyrebox_wirelog_derived_membership_clear (membership);
  g_free (membership);
}

static gboolean
csv_newline_at (const char *csv, gsize len, gsize index, gsize *advance)
{
  if (csv[index] == '\n') {
    *advance = 1;
    return TRUE;
  }

  if (csv[index] == '\r') {
    *advance = (index + 1 < len && csv[index + 1] == '\n') ? 2 : 1;
    return TRUE;
  }

  return FALSE;
}

static void
append_field (GPtrArray *row, GString *field)
{
  g_ptr_array_add (row, g_strdup (field->str));
  g_string_truncate (field, 0);
}

static gboolean
append_record (GPtrArray *memberships, GPtrArray *row, GError **error)
{
  WyreboxWirelogDerivedMembership *membership = NULL;
  const char *view_id = NULL;
  const char *message_id = NULL;

  if (row->len != 2) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Derived membership CSV row must have exactly two columns");
    return FALSE;
  }

  view_id = g_ptr_array_index (row, 0);
  message_id = g_ptr_array_index (row, 1);

  if (view_id[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Derived membership CSV row has an empty view_id");
    return FALSE;
  }

  if (message_id[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Derived membership CSV row has an empty message_id");
    return FALSE;
  }

  membership = g_new0 (WyreboxWirelogDerivedMembership, 1);
  membership->view_id = g_strdup (view_id);
  membership->message_id = g_strdup (message_id);
  g_ptr_array_add (memberships, membership);

  g_ptr_array_set_size (row, 0);
  return TRUE;
}

GPtrArray *
wyrebox_wirelog_derived_membership_parse_csv (const char *csv, GError **error)
{
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GPtrArray) row = NULL;
  g_autoptr (GString) field = NULL;
  gboolean in_quotes = FALSE;
  gboolean quoted_field = FALSE;
  gboolean after_quote = FALSE;
  gboolean have_field = FALSE;
  gsize len = 0;
  gsize i = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (csv == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Derived membership CSV text is required");
    return NULL;
  }

  memberships = membership_array_new ();
  row = g_ptr_array_new_with_free_func (g_free);
  field = g_string_new (NULL);
  len = strlen (csv);

  while (i < len) {
    gsize newline_advance = 0;
    char c = csv[i];

    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < len && csv[i + 1] == '"') {
          g_string_append_c (field, '"');
          i += 2;
        } else {
          in_quotes = FALSE;
          after_quote = TRUE;
          i++;
        }
      } else {
        g_string_append_c (field, c);
        i++;
      }
      continue;
    }

    if (after_quote) {
      if (c == ',') {
        append_field (row, field);
        have_field = FALSE;
        quoted_field = FALSE;
        after_quote = FALSE;
        i++;
        continue;
      }

      if (csv_newline_at (csv, len, i, &newline_advance)) {
        append_field (row, field);
        if (!append_record (memberships, row, error))
          return NULL;
        have_field = FALSE;
        quoted_field = FALSE;
        after_quote = FALSE;
        i += newline_advance;
        continue;
      }

      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "Malformed derived membership CSV quoted field");
      return NULL;
    }

    if (c == ',' || csv_newline_at (csv, len, i, &newline_advance)) {
      append_field (row, field);
      if (c != ',') {
        if (!append_record (memberships, row, error))
          return NULL;
        i += newline_advance;
      } else {
        i++;
      }
      have_field = FALSE;
      quoted_field = FALSE;
      continue;
    }

    if (c == '"') {
      if (have_field || quoted_field) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "Malformed derived membership CSV quote");
        return NULL;
      }

      quoted_field = TRUE;
      in_quotes = TRUE;
      i++;
      continue;
    }

    have_field = TRUE;
    g_string_append_c (field, c);
    i++;
  }

  if (in_quotes) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Malformed derived membership CSV unterminated quoted field");
    return NULL;
  }

  if (after_quote || have_field || quoted_field || row->len > 0) {
    append_field (row, field);
    if (!append_record (memberships, row, error))
      return NULL;
  }

  return g_steal_pointer (&memberships);
}

GPtrArray *wyrebox_wirelog_derived_membership_parse_evaluation_relation
    (WyreboxWirelogEvaluation * evaluation, const char *relation_name,
    GError ** error)
{
  g_autofree char *csv = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!wyrebox_wirelog_evaluation_export_relation_csv (evaluation,
          relation_name, &csv, error))
    return NULL;

  return wyrebox_wirelog_derived_membership_parse_csv (csv, error);
}
