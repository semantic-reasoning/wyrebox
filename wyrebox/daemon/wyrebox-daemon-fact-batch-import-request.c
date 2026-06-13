#include "wyrebox-daemon-fact-batch-import-request.h"

#include <gio/gio.h>

static void
free_fact_mutation_request (gpointer data)
{
  WyreboxDaemonFactMutationRequest *request = data;

  if (request == NULL)
    return;

  wyrebox_daemon_fact_mutation_request_clear (request);
  g_free (request);
}

static gboolean
copy_fact_mutation_request (const WyreboxDaemonFactMutationRequest *src,
    WyreboxDaemonFactMutationRequest **out_copy, GError **error)
{
  WyreboxDaemonFactMutationRequest *copy = NULL;

  g_assert (out_copy != NULL);
  *out_copy = NULL;

  if (src == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import entries must not contain NULL requests");
    return FALSE;
  }

  copy = g_new0 (WyreboxDaemonFactMutationRequest, 1);
  if (!wyrebox_daemon_fact_mutation_request_init (copy,
          src->mutation, src->predicate_id, src->scope_id,
          (const char *const *) src->arguments, error)) {
    wyrebox_daemon_fact_mutation_request_clear (copy);
    g_free (copy);
    return FALSE;
  }

  *out_copy = copy;
  return TRUE;
}

static gboolean
validate_entry_scope (const WyreboxDaemonFactMutationRequest *entry,
    const char *expected_scope_id, GError **error)
{
  if (entry == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import contains an invalid entry");
    return FALSE;
  }

  if (entry->scope_id == NULL || *entry->scope_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import entry scope_id is required");
    return FALSE;
  }

  if (g_strcmp0 (entry->scope_id, expected_scope_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import entries must share one scope_id");
    return FALSE;
  }

  return TRUE;
}

void wyrebox_daemon_fact_batch_import_request_clear
    (WyreboxDaemonFactBatchImportRequest * request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->entries, g_ptr_array_unref);
  g_clear_pointer (&request->scope_id, g_free);
}

gboolean
    wyrebox_daemon_fact_batch_import_request_init
    (WyreboxDaemonFactBatchImportRequest * request,
    const WyreboxDaemonFactMutationRequest * const *entries, guint n_entries,
    GError ** error)
{
  g_auto (WyreboxDaemonFactBatchImportRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (entries == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import entries vector is required");
    return FALSE;
  }

  if (n_entries == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import requires at least one entry");
    return FALSE;
  }

  if (n_entries > WYREBOX_DAEMON_FACT_BATCH_IMPORT_REQUEST_MAX_ENTRIES) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact batch import exceeds max entries");
    return FALSE;
  }

  next.entries = g_ptr_array_new_with_free_func (free_fact_mutation_request);

  for (guint i = 0; i < n_entries; i++) {
    WyreboxDaemonFactMutationRequest *copy = NULL;

    if (!copy_fact_mutation_request (entries[i], &copy, error))
      return FALSE;

    if (i == 0)
      next.scope_id = g_strdup (copy->scope_id);
    else if (!validate_entry_scope (copy, next.scope_id, error)) {
      free_fact_mutation_request (copy);
      return FALSE;
    }

    g_ptr_array_add (next.entries, copy);
  }

  wyrebox_daemon_fact_batch_import_request_clear (request);
  *request = next;
  next.entries = NULL;
  next.scope_id = NULL;

  return TRUE;
}

guint
    wyrebox_daemon_fact_batch_import_request_get_n_entries
    (const WyreboxDaemonFactBatchImportRequest * request)
{
  if (request == NULL || request->entries == NULL)
    return 0;

  return request->entries->len;
}

const WyreboxDaemonFactMutationRequest *
wyrebox_daemon_fact_batch_import_request_get_entry (const
    WyreboxDaemonFactBatchImportRequest *request, guint index)
{
  if (request == NULL || request->entries == NULL ||
      index >= request->entries->len)
    return NULL;

  return g_ptr_array_index (request->entries, index);
}

const char *wyrebox_daemon_fact_batch_import_request_get_scope_id
    (const WyreboxDaemonFactBatchImportRequest * request)
{
  if (request == NULL)
    return NULL;

  return request->scope_id;
}

gboolean
    wyrebox_daemon_fact_batch_import_request_validate
    (const WyreboxDaemonFactBatchImportRequest * request, GError ** error)
{
  guint n_entries = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (request == NULL || request->entries == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact batch import request is required");
    return FALSE;
  }

  n_entries = request->entries->len;
  if (n_entries == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import requires at least one entry");
    return FALSE;
  }

  if (n_entries > WYREBOX_DAEMON_FACT_BATCH_IMPORT_REQUEST_MAX_ENTRIES) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact batch import exceeds max entries");
    return FALSE;
  }

  if (request->scope_id == NULL || *request->scope_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact batch import scope_id is required");
    return FALSE;
  }

  for (guint i = 0; i < n_entries; i++) {
    const WyreboxDaemonFactMutationRequest *entry =
        g_ptr_array_index (request->entries, i);
    WyreboxJournalEventType event_type =
        WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
    g_autoptr (GBytes) payload = NULL;

    if (!validate_entry_scope (entry, request->scope_id, error))
      return FALSE;

    if (!wyrebox_daemon_fact_mutation_request_get_event (entry,
            &event_type, error))
      return FALSE;

    payload = wyrebox_daemon_fact_mutation_request_encode (entry, error);
    if (payload == NULL)
      return FALSE;
  }

  return TRUE;
}
