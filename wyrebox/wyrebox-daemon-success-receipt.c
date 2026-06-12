#include "wyrebox-daemon-success-receipt.h"

#include <gio/gio.h>

void
wyrebox_daemon_success_receipt_clear (WyreboxDaemonSuccessReceipt *receipt)
{
  if (receipt == NULL)
    return;

  g_clear_pointer (&receipt->request_id, g_free);
  g_clear_pointer (&receipt->durable_marker, g_free);
  receipt->journal_offset = 0;
  receipt->journal_sequence = 0;
  g_clear_pointer (&receipt->summary, g_free);
}

gboolean
    wyrebox_daemon_success_receipt_init_delivery_ingestion
    (WyreboxDaemonSuccessReceipt * receipt, const char *request_id,
    const WyreboxEmlIngestResult * ingest_result, GError ** error)
{
  g_auto (WyreboxDaemonSuccessReceipt) next = { 0 };

  g_return_val_if_fail (receipt != NULL, FALSE);
  g_return_val_if_fail (ingest_result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (request_id == NULL || *request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "delivery success requires request_id");
    return FALSE;
  }

  if (ingest_result->object_key == NULL || *ingest_result->object_key == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "delivery success requires an immutable object key");
    return FALSE;
  }

  if (ingest_result->journal_sequence == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "delivery success requires a durable journal sequence");
    return FALSE;
  }

  next.request_id = g_strdup (request_id);
  next.durable_marker = g_strdup_printf ("journal:%" G_GUINT64_FORMAT ":%"
      G_GUINT64_FORMAT, ingest_result->journal_offset,
      ingest_result->journal_sequence);
  next.journal_offset = ingest_result->journal_offset;
  next.journal_sequence = ingest_result->journal_sequence;
  next.summary = g_strdup_printf ("delivery_ingestion object_key=%s "
      "size_bytes=%" G_GUINT64_FORMAT,
      ingest_result->object_key, ingest_result->size_bytes);

  wyrebox_daemon_success_receipt_clear (receipt);
  *receipt = next;
  next.request_id = NULL;
  next.durable_marker = NULL;
  next.journal_offset = 0;
  next.journal_sequence = 0;
  next.summary = NULL;

  return TRUE;
}

static guint
count_fact_arguments (char **arguments)
{
  guint count = 0;

  while (arguments[count] != NULL)
    count++;

  return count;
}

gboolean
wyrebox_daemon_success_receipt_init_fact_mutation (WyreboxDaemonSuccessReceipt
    *receipt, const char *request_id,
    const WyreboxDaemonFactMutationRequest *request, guint64 journal_offset,
    guint64 journal_sequence, GError **error)
{
  WyreboxJournalEventType event_type = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
  g_auto (WyreboxDaemonSuccessReceipt) next = { 0 };

  g_return_val_if_fail (receipt != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (request_id == NULL || *request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation success requires request_id");
    return FALSE;
  }

  if (!wyrebox_daemon_fact_mutation_request_get_event (request,
          &event_type, error))
    return FALSE;

  if (journal_sequence == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "fact mutation success requires a durable journal sequence");
    return FALSE;
  }

  next.request_id = g_strdup (request_id);
  next.durable_marker = g_strdup_printf ("journal:%" G_GUINT64_FORMAT ":%"
      G_GUINT64_FORMAT, journal_offset, journal_sequence);
  next.journal_offset = journal_offset;
  next.journal_sequence = journal_sequence;
  next.summary = g_strdup_printf ("fact_mutation mutation=%s "
      "predicate_id=%s scope_id=%s argument_count=%u",
      wyrebox_daemon_fact_mutation_kind_to_wire_name (request->mutation),
      request->predicate_id,
      request->scope_id, count_fact_arguments (request->arguments));

  wyrebox_daemon_success_receipt_clear (receipt);
  *receipt = next;
  next.request_id = NULL;
  next.durable_marker = NULL;
  next.journal_offset = 0;
  next.journal_sequence = 0;
  next.summary = NULL;

  return TRUE;
}
