#include "wyrebox-wirelog-derived-membership.h"

#include "wyrebox-wirelog-source.h"

#include <gio/gio.h>
#include <wirelog/wirelog.h>

typedef struct
{
  GPtrArray *memberships;
  GHashTable *known_symbols_by_id;
  GError *error;
} SnapshotContext;

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

static gint
membership_compare (gconstpointer left, gconstpointer right)
{
  const WyreboxWirelogDerivedMembership *left_membership =
      *(WyreboxWirelogDerivedMembership * const *) left;
  const WyreboxWirelogDerivedMembership *right_membership =
      *(WyreboxWirelogDerivedMembership * const *) right;
  gint view_compare = 0;

  view_compare = g_strcmp0 (left_membership->view_id,
      right_membership->view_id);
  if (view_compare != 0)
    return view_compare;

  return g_strcmp0 (left_membership->message_id, right_membership->message_id);
}

static guint
int64_hash (gconstpointer value)
{
  guint64 bits = (guint64) * (const gint64 *) value;

  return (guint) (bits ^ (bits >> 32));
}

static gboolean
int64_equal (gconstpointer left, gconstpointer right)
{
  return *(const gint64 *) left == *(const gint64 *) right;
}

static const char *
wirelog_error_name (wirelog_error_t error)
{
  switch (error) {
    case WIRELOG_OK:
      return "WIRELOG_OK";
    case WIRELOG_ERR_PARSE:
      return "WIRELOG_ERR_PARSE";
    case WIRELOG_ERR_INVALID_IR:
      return "WIRELOG_ERR_INVALID_IR";
    case WIRELOG_ERR_EXEC:
      return "WIRELOG_ERR_EXEC";
    case WIRELOG_ERR_MEMORY:
      return "WIRELOG_ERR_MEMORY";
    case WIRELOG_ERR_IO:
      return "WIRELOG_ERR_IO";
    case WIRELOG_ERR_COMPOUND_SATURATED:
      return "WIRELOG_ERR_COMPOUND_SATURATED";
    case WIRELOG_ERR_COMPOUND_BUSY:
      return "WIRELOG_ERR_COMPOUND_BUSY";
    case WIRELOG_ERR_UNKNOWN:
      return "WIRELOG_ERR_UNKNOWN";
    default:
      return "unknown Wirelog error";
  }
}

static gboolean
remember_known_symbol (GHashTable *known_symbols_by_id, gint64 id,
    const char *symbol, GError **error)
{
  const char *existing = NULL;
  gint64 *key = NULL;

  if (id < 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "Wirelog returned a negative symbol id while interning \"%s\"", symbol);
    return FALSE;
  }

  existing = g_hash_table_lookup (known_symbols_by_id, &id);
  if (existing != NULL) {
    if (g_strcmp0 (existing, symbol) == 0)
      return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Wirelog symbol id %" G_GINT64_FORMAT
        " maps to both \"%s\" and \"%s\"", id, existing, symbol);
    return FALSE;
  }

  key = g_new (gint64, 1);
  *key = id;
  g_hash_table_insert (known_symbols_by_id, key, g_strdup (symbol));
  return TRUE;
}

static gboolean
preintern_fact_symbols (wirelog_easy_session_t *session,
    GHashTable *known_symbols_by_id, GPtrArray *facts, GError **error)
{
  for (guint index = 0; index < facts->len; index++) {
    const WyreboxFactRecord *record = g_ptr_array_index (facts, index);

    if (record == NULL || record->args == NULL)
      continue;

    for (guint arg_index = 0; record->args[arg_index] != NULL; arg_index++) {
      const char *arg = record->args[arg_index];
      gint64 id = 0;

      if (arg[0] == '\0')
        continue;

      id = wirelog_easy_intern (session, arg);
      if (!remember_known_symbol (known_symbols_by_id, id, arg, error))
        return FALSE;
    }
  }

  return TRUE;
}

static gboolean
lookup_snapshot_symbol (GHashTable *known_symbols_by_id, gint64 id,
    const char *field_name, const char **out_symbol, GError **error)
{
  const char *symbol = NULL;

  if (id < 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Wirelog derived membership %s id is negative: %" G_GINT64_FORMAT,
        field_name, id);
    return FALSE;
  }

  symbol = g_hash_table_lookup (known_symbols_by_id, &id);
  if (symbol == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "Wirelog derived membership %s id %" G_GINT64_FORMAT
        " is not in the WyreBox known-symbol map", field_name, id);
    return FALSE;
  }

  *out_symbol = symbol;
  return TRUE;
}

static void
snapshot_membership_cb (const char *relation,
    const int64_t *row, uint32_t ncols, void *user_data)
{
  SnapshotContext *context = user_data;
  const char *view_id = NULL;
  const char *message_id = NULL;
  WyreboxWirelogDerivedMembership *membership = NULL;

  (void) relation;

  if (context->error != NULL)
    return;

  if (row == NULL || ncols != 2) {
    g_set_error (&context->error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Wirelog derived membership snapshot row must have exactly two "
        "columns");
    return;
  }

  if (!lookup_snapshot_symbol (context->known_symbols_by_id, row[0],
          "view_id", &view_id, &context->error) ||
      !lookup_snapshot_symbol (context->known_symbols_by_id, row[1],
          "message_id", &message_id, &context->error))
    return;

  membership = g_new0 (WyreboxWirelogDerivedMembership, 1);
  membership->view_id = g_strdup (view_id);
  membership->message_id = g_strdup (message_id);
  g_ptr_array_add (context->memberships, membership);
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

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "Wirelog result CSV does not preserve symbol strings for derived "
      "membership extraction");
  return NULL;
}

GPtrArray *wyrebox_wirelog_derived_membership_snapshot_from_rules_and_facts
    (const char *rules_source, GPtrArray * facts, const char *relation_name,
    GError ** error)
{
  g_autofree char *source = NULL;
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GHashTable) known_symbols_by_id = NULL;
  wirelog_easy_session_t *session = NULL;
  wirelog_error_t wirelog_error = WIRELOG_OK;
  SnapshotContext context = { 0 };

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (relation_name == NULL || relation_name[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Wirelog derived membership relation name is required");
    return NULL;
  }

  source = wyrebox_wirelog_source_build (rules_source, facts, error);
  if (source == NULL)
    return NULL;

  wirelog_error = wirelog_easy_open (source, &session);
  if (wirelog_error != WIRELOG_OK || session == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Wirelog easy session open failed: %s",
        wirelog_error_name (wirelog_error));
    return NULL;
  }

  memberships = membership_array_new ();
  known_symbols_by_id = g_hash_table_new_full (int64_hash, int64_equal,
      g_free, g_free);

  if (!preintern_fact_symbols (session, known_symbols_by_id, facts, error))
    goto fail;

  context.memberships = memberships;
  context.known_symbols_by_id = known_symbols_by_id;
  wirelog_error = wirelog_easy_snapshot (session, relation_name,
      snapshot_membership_cb, &context);
  if (context.error != NULL) {
    g_propagate_error (error, g_steal_pointer (&context.error));
    goto fail;
  }
  if (wirelog_error != WIRELOG_OK) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "Wirelog easy snapshot failed for relation \"%s\": %s",
        relation_name, wirelog_error_name (wirelog_error));
    goto fail;
  }

  g_ptr_array_sort (memberships, membership_compare);

  wirelog_easy_close (session);
  return g_steal_pointer (&memberships);

fail:
  wirelog_easy_close (session);
  return NULL;
}
