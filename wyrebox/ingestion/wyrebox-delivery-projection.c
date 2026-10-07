#include "wyrebox-delivery-projection.h"

#include "wyrebox-delivery-object-check.h"
#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>

#include <string.h>

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

    G_OBJECT_CLASS (wyrebox_delivery_projection_parent_class)->finalize (
        object);
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
    g_clear_pointer (&record->account_identity, g_free);
    g_clear_pointer (&record->rfc_message_id, g_free);
    g_clear_pointer (&record->subject, g_free);
    g_clear_pointer (&record->from, g_free);
    g_clear_pointer (&record->to, g_free);
    g_clear_pointer (&record->cc, g_free);
    g_clear_pointer (&record->bcc, g_free);
    g_clear_pointer (&record->date_raw, g_free);
    record->message_id_span_valid = FALSE;
    record->message_id_span_start = 0;
    record->message_id_span_end = 0;
    record->subject_span_valid = FALSE;
    record->subject_span_start = 0;
    record->subject_span_end = 0;
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
    projection.account_identity = g_steal_pointer (&payload->account_identity);
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
    projection.message_id_span_valid = payload->message_id_span_valid;
    projection.message_id_span_start = payload->message_id_span_start;
    projection.message_id_span_end = payload->message_id_span_end;
    projection.subject_span_valid = payload->subject_span_valid;
    projection.subject_span_start = payload->subject_span_start;
    projection.subject_span_end = payload->subject_span_end;

    entry = g_new0 (WyreboxDeliveryProjectionRecord, 1);
    *entry = projection;
    memset (&projection, 0, sizeof (projection));

    g_ptr_array_add (out_projection->records, entry);
}

gboolean
wyrebox_delivery_projection_check_record_object (WyreboxLocalObjectStore
    *object_store, const WyreboxDeliveryProjectionRecord *record,
    GError **error)
{
    g_autoptr (GError) local_error = NULL;

    g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), FALSE);
    g_return_val_if_fail (record != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    switch (wyrebox_delivery_object_check (object_store, record->object_key,
        record->size_bytes, &local_error)) {
    case WYREBOX_DELIVERY_OBJECT_CHECK_OK:
        return TRUE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_MISSING:
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MessageDelivered record at sequence %" G_GUINT64_FORMAT
            " references unavailable raw object %s: %s",
            record->journal_sequence, record->object_key,
            local_error->message);
        return FALSE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_SIZE_MISMATCH:
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MessageDelivered record at sequence %" G_GUINT64_FORMAT ": %s",
            record->journal_sequence, local_error->message);
        return FALSE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_HASH_MISMATCH:
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MessageDelivered record at sequence %" G_GUINT64_FORMAT
            " references raw object %s with SHA-256 mismatch",
            record->journal_sequence, record->object_key);
        return FALSE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_INVALID_KEY:
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MessageDelivered record at sequence %" G_GUINT64_FORMAT
            " references invalid raw object key %s: %s",
            record->journal_sequence, record->object_key,
            local_error->message);
        return FALSE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_UNREADABLE:
    default:
        g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
            "MessageDelivered record at sequence %" G_GUINT64_FORMAT
            ": failed to read raw object %s: ", record->journal_sequence,
            record->object_key);
        return FALSE;
    }
}

gboolean
wyrebox_delivery_projection_replay_records (WyreboxDeliveryProjection *self,
    WyreboxDeliveryProjectionList *out_projection, GError **error)
{
    g_auto (WyreboxJournalRecord) record = { 0 };
    gboolean eof = FALSE;

    g_return_val_if_fail (WYREBOX_IS_DELIVERY_PROJECTION (self), FALSE);
    g_return_val_if_fail (out_projection != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    wyrebox_delivery_projection_list_clear (out_projection);
    out_projection->records = g_ptr_array_new_with_free_func (
        (GDestroyNotify)delivery_projection_record_free);

    while (TRUE) {
        g_autoptr (GError) local_error = NULL;
        g_auto (WyreboxMessageDeliveredPayload) payload = { 0 };

        if (!wyrebox_journal_reader_read_next (self->journal_reader,
            &record, &eof, &local_error)) {
            if (eof)
                return TRUE;

            g_set_error (error,
                local_error != NULL ? local_error->domain : G_IO_ERROR,
                local_error !=
                NULL ? local_error->code : G_IO_ERROR_INVALID_DATA,
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

        append_delivered_record (out_projection, &record, &payload);
    }
}

gboolean
wyrebox_delivery_projection_replay_all (WyreboxDeliveryProjection *self,
    WyreboxDeliveryProjectionList *out_projection, GError **error)
{
    g_return_val_if_fail (WYREBOX_IS_DELIVERY_PROJECTION (self), FALSE);
    g_return_val_if_fail (out_projection != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!wyrebox_delivery_projection_replay_records (self, out_projection,
        error))
        return FALSE;

    for (guint i = 0; i < out_projection->records->len; i++) {
        if (!wyrebox_delivery_projection_check_record_object (
                self->object_store,
                g_ptr_array_index (out_projection->records, i), error)) {
            wyrebox_delivery_projection_list_clear (out_projection);
            return FALSE;
        }
    }

    return TRUE;
}
