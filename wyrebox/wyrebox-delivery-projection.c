#include "wyrebox-delivery-projection.h"

#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>

struct _WyreboxDeliveryProjection
{
  GObject parent_instance;

  WyreboxJournalReader *journal_reader;
  WyreboxLocalObjectStore *object_store;
};

G_DEFINE_TYPE (WyreboxDeliveryProjection, wyrebox_delivery_projection,
    G_TYPE_OBJECT);

static void
delivery_projection_record_free (gpointer data)
{
  WyreboxDeliveryProjectionRecord *record = data;

  if (record == NULL)
    return;

  wyrebox_delivery_projection_record_clear (record);
  g_free (record);
}

static void
wyrebox_delivery_projection_finalize (GObject *object)
{
  WyreboxDeliveryProjection *self = WYREBOX_DELIVERY_PROJECTION (object);

  g_clear_object (&self->journal_reader);
  g_clear_object (&self->object_store);

  G_OBJECT_CLASS (wyrebox_delivery_projection_parent_class)->finalize (object);
}

static void
wyrebox_delivery_projection_class_init (WyreboxDeliveryProjectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_delivery_projection_finalize;
}

static void
wyrebox_delivery_projection_init (WyreboxDeliveryProjection *self)
{
}

void
wyrebox_delivery_projection_record_clear (WyreboxDeliveryProjectionRecord
    *record)
{
  if (record == NULL)
    return;

  g_clear_pointer (&record->object_key, g_free);
  g_clear_pointer (&record->rfc_message_id, g_free);
  g_clear_pointer (&record->subject, g_free);
  g_clear_pointer (&record->from, g_free);
  g_clear_pointer (&record->to, g_free);
  g_clear_pointer (&record->cc, g_free);
  g_clear_pointer (&record->bcc, g_free);
  g_clear_pointer (&record->date_raw, g_free);
  record->size_bytes = 0;
  record->internal_date_unix_us = 0;
  record->duplicate_message_id_count = 0;
  record->journal_offset = 0;
  record->journal_sequence = 0;
}

void
wyrebox_delivery_projection_list_clear (WyreboxDeliveryProjectionList
    *projection)
{
  if (projection == NULL)
    return;

  g_clear_pointer (&projection->records, g_ptr_array_unref);
  projection->records = NULL;
}

WyreboxDeliveryProjection *
wyrebox_delivery_projection_new (WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store)
{
  g_autoptr (WyreboxDeliveryProjection) self = NULL;

  g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (journal_reader), NULL);
  g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), NULL);

  self = g_object_new (WYREBOX_TYPE_DELIVERY_PROJECTION, NULL);
  self->journal_reader = g_object_ref (journal_reader);
  self->object_store = g_object_ref (object_store);

  return g_steal_pointer (&self);
}

static void
append_delivered_record (WyreboxDeliveryProjectionList *out_projection,
    WyreboxJournalRecord *record, WyreboxMessageDeliveredPayload *payload)
{
  g_auto (WyreboxDeliveryProjectionRecord) projection = { 0 };
  WyreboxDeliveryProjectionRecord *entry = NULL;

  projection.journal_offset = record->offset;
  projection.journal_sequence = record->sequence;
  projection.object_key = g_steal_pointer (&payload->object_key);
  projection.size_bytes = payload->size_bytes;
  projection.internal_date_unix_us = payload->internal_date_unix_us;
  projection.duplicate_message_id_count = payload->duplicate_message_id_count;
  projection.rfc_message_id = g_steal_pointer (&payload->message_id);
  projection.subject = g_steal_pointer (&payload->subject);
  projection.from = g_steal_pointer (&payload->from);
  projection.to = g_steal_pointer (&payload->to);
  projection.cc = g_steal_pointer (&payload->cc);
  projection.bcc = g_steal_pointer (&payload->bcc);
  projection.date_raw = g_steal_pointer (&payload->date);

  entry = g_new0 (WyreboxDeliveryProjectionRecord, 1);
  *entry = projection;
  memset (&projection, 0, sizeof (projection));

  g_ptr_array_add (out_projection->records, entry);
}

gboolean
wyrebox_delivery_projection_replay_all (WyreboxDeliveryProjection *self,
    WyreboxDeliveryProjectionList *out_projection, GError **error)
{
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  g_return_val_if_fail (WYREBOX_IS_DELIVERY_PROJECTION (self), FALSE);
  g_return_val_if_fail (out_projection != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  wyrebox_delivery_projection_list_clear (out_projection);
  out_projection->records = g_ptr_array_new_with_free_func (
      (GDestroyNotify) delivery_projection_record_free);

  while (TRUE) {
    g_autoptr (GError) local_error = NULL;
    g_autoptr (GBytes) object_bytes = NULL;
    g_auto (WyreboxMessageDeliveredPayload) payload = { 0 };

    if (!wyrebox_journal_reader_read_next (self->journal_reader,
            &record, &eof, &local_error)) {
      if (eof)
        return TRUE;

      g_set_error (error,
          local_error != NULL ? local_error->domain : G_IO_ERROR,
          local_error != NULL ? local_error->code : G_IO_ERROR_INVALID_DATA,
          "failed to read journal at sequence %" G_GUINT64_FORMAT
          ": %s",
          record.sequence,
          local_error != NULL ? local_error->message : "unknown error");
      wyrebox_delivery_projection_list_clear (out_projection);
      return FALSE;
    }

    if (record.event_type != WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED)
      continue;

    if (!wyrebox_message_delivered_payload_decode (record.payload,
            &payload, &local_error)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "failed to decode MessageDelivered payload at sequence %"
          G_GUINT64_FORMAT ": %s",
          record.sequence,
          local_error != NULL ? local_error->message : "unknown error");
      wyrebox_delivery_projection_list_clear (out_projection);
      return FALSE;
    }

    object_bytes = wyrebox_local_object_store_get_bytes (self->object_store,
        payload.object_key, &local_error);
    if (object_bytes == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "MessageDelivered record at sequence %" G_GUINT64_FORMAT
          " references unavailable raw object %s: %s",
          record.sequence,
          payload.object_key,
          local_error != NULL ? local_error->message : "unknown error");
      wyrebox_delivery_projection_list_clear (out_projection);
      return FALSE;
    }

    append_delivered_record (out_projection, &record, &payload);
  }
}
