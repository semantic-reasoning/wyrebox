#include "wyrebox-fact-journal-snapshot.h"

#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-journal-reader.h"

#include <string.h>

#define WYREBOX_FACT_CONFIDENCE_EXACT 1000000u

static void
fact_record_ptr_free (gpointer data)
{
  WyreboxFactRecord *record = data;

  if (record == NULL)
    return;

  wyrebox_fact_record_clear (record);
  g_free (record);
}

static void
append_key_component (GString *key, const char *value)
{
  g_string_append_printf (key, "%" G_GSIZE_FORMAT ":", strlen (value));
  g_string_append (key, value);
}

static char *
build_fact_identity_key (const char *predicate, const char *source, char **args)
{
  g_autoptr (GString) key = NULL;

  key = g_string_new (NULL);
  append_key_component (key, predicate);
  append_key_component (key, source);
  for (guint i = 0; args[i] != NULL; i++)
    append_key_component (key, args[i]);

  return g_string_free (g_steal_pointer (&key), FALSE);
}

static int
compare_nullable_string (const char *left, const char *right)
{
  if (left == NULL && right == NULL)
    return 0;
  if (left == NULL)
    return -1;
  if (right == NULL)
    return 1;
  return strcmp (left, right);
}

static int
compare_args (char **left, char **right)
{
  guint i = 0;

  for (i = 0; left[i] != NULL && right[i] != NULL; i++) {
    int cmp = strcmp (left[i], right[i]);

    if (cmp != 0)
      return cmp;
  }

  if (left[i] == NULL && right[i] == NULL)
    return 0;
  return left[i] == NULL ? -1 : 1;
}

static gint
compare_fact_records (gconstpointer left_data, gconstpointer right_data)
{
  const WyreboxFactRecord *left = left_data;
  const WyreboxFactRecord *right = right_data;
  int cmp = 0;

  cmp = compare_nullable_string (left->predicate, right->predicate);
  if (cmp != 0)
    return cmp;

  cmp = compare_nullable_string (left->source, right->source);
  if (cmp != 0)
    return cmp;

  return compare_args (left->args, right->args);
}

static gboolean
record_event_matches_request (WyreboxJournalEventType event_type,
    const WyreboxDaemonFactMutationRequest *request, GError **error)
{
  WyreboxJournalEventType request_event =
      WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;

  if (!wyrebox_daemon_fact_mutation_request_get_event (request,
          &request_event, error))
    return FALSE;

  if (event_type == request_event)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "fact mutation journal event does not match payload mutation");
  return FALSE;
}

static gboolean
apply_fact_insert (GHashTable *active,
    const WyreboxDaemonFactMutationRequest *request,
    guint64 sequence, GError **error)
{
  g_autofree char *source = NULL;
  g_autofree char *key = NULL;
  WyreboxFactRecord *record = NULL;

  source = g_strdup_printf ("fact-mutation:%s", request->scope_id);
  key = build_fact_identity_key (request->predicate_id, source,
      request->arguments);
  if (g_hash_table_contains (active, key))
    return TRUE;

  record = g_new0 (WyreboxFactRecord, 1);
  if (!wyrebox_fact_record_init (record, request->predicate_id,
          (const char *const *) request->arguments, source,
          WYREBOX_FACT_CONFIDENCE_EXACT, sequence, error)) {
    fact_record_ptr_free (record);
    return FALSE;
  }

  g_hash_table_insert (active, g_steal_pointer (&key), record);
  return TRUE;
}

static void
apply_fact_retract (GHashTable *active,
    const WyreboxDaemonFactMutationRequest *request)
{
  g_autofree char *source = NULL;
  g_autofree char *key = NULL;

  source = g_strdup_printf ("fact-mutation:%s", request->scope_id);
  key = build_fact_identity_key (request->predicate_id, source,
      request->arguments);
  g_hash_table_remove (active, key);
}

static gboolean
apply_fact_mutation_record (GHashTable *active,
    const WyreboxJournalRecord *record, GError **error)
{
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };

  if (!wyrebox_daemon_fact_mutation_request_decode (record->payload, &request,
          error))
    return FALSE;

  if (!record_event_matches_request (record->event_type, &request, error))
    return FALSE;

  switch (record->event_type) {
    case WYREBOX_JOURNAL_EVENT_FACT_INSERTED:
      return apply_fact_insert (active, &request, record->sequence, error);
    case WYREBOX_JOURNAL_EVENT_FACT_RETRACTED:
      apply_fact_retract (active, &request);
      return TRUE;
    default:
      g_assert_not_reached ();
  }
}

static GPtrArray *
active_table_to_sorted_array (GHashTable *active)
{
  g_autoptr (GList) values = NULL;
  GPtrArray *records = NULL;

  records = g_ptr_array_new_with_free_func (fact_record_ptr_free);
  values = g_hash_table_get_values (active);
  values = g_list_sort (g_steal_pointer (&values), compare_fact_records);

  for (GList * iter = values; iter != NULL; iter = iter->next) {
    WyreboxFactRecord *record = iter->data;
    g_autofree char *key = NULL;

    key = build_fact_identity_key (record->predicate, record->source,
        record->args);
    g_assert_true (g_hash_table_steal (active, key));
    g_ptr_array_add (records, record);
  }

  return records;
}

GPtrArray *
wyrebox_fact_journal_snapshot_load_active (const char *journal_root_dir,
    GError **error)
{
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GHashTable) active = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  reader = wyrebox_journal_reader_new (journal_root_dir, error);
  if (reader == NULL)
    return NULL;

  active = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      fact_record_ptr_free);

  for (;;) {
    g_auto (WyreboxJournalRecord) record = { 0 };
    gboolean eof = FALSE;

    if (!wyrebox_journal_reader_read_next (reader, &record, &eof, error)) {
      if (eof)
        break;
      return NULL;
    }

    if (record.event_type != WYREBOX_JOURNAL_EVENT_FACT_INSERTED &&
        record.event_type != WYREBOX_JOURNAL_EVENT_FACT_RETRACTED)
      continue;

    if (!apply_fact_mutation_record (active, &record, error))
      return NULL;
  }

  return active_table_to_sorted_array (active);
}
