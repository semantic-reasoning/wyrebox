#include "wyrebox-delivery-replay-validator.h"

#include "wyrebox-delivery-object-check.h"
#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_DEFINE_QUARK (wyrebox-delivery-replay-validator-error,
    wyrebox_delivery_replay_validator_error);
/* *INDENT-ON* */

struct _WyreboxDeliveryReplayValidator
{
    GObject parent_instance;

    WyreboxJournalReader *journal_reader;
    WyreboxLocalObjectStore *object_store;
};

G_DEFINE_TYPE (WyreboxDeliveryReplayValidator,
    wyrebox_delivery_replay_validator, G_TYPE_OBJECT);

static void
wyrebox_delivery_replay_validator_finalize (GObject *object)
{
    WyreboxDeliveryReplayValidator *self =
        WYREBOX_DELIVERY_REPLAY_VALIDATOR (object);

    g_clear_object (&self->journal_reader);
    g_clear_object (&self->object_store);

    G_OBJECT_CLASS (wyrebox_delivery_replay_validator_parent_class)->finalize
        (object);
}

static void
wyrebox_delivery_replay_validator_class_init
    (WyreboxDeliveryReplayValidatorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = wyrebox_delivery_replay_validator_finalize;
}

static void
wyrebox_delivery_replay_validator_init (WyreboxDeliveryReplayValidator *self)
{
}

WyreboxDeliveryReplayValidator *
wyrebox_delivery_replay_validator_new (WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store)
{
    g_autoptr (WyreboxDeliveryReplayValidator) self = NULL;

    g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (journal_reader), NULL);
    g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), NULL);

    self = g_object_new (WYREBOX_TYPE_DELIVERY_REPLAY_VALIDATOR, NULL);
    self->journal_reader = g_object_ref (journal_reader);
    self->object_store = g_object_ref (object_store);

    return g_steal_pointer (&self);
}

static gboolean
is_counted_object_failure (const GError *error)
{
    return g_error_matches (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
               WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_MISSING_OBJECT) ||
           g_error_matches (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
               WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_SIZE_MISMATCH) ||
           g_error_matches (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
               WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_HASH_MISMATCH);
}

static gboolean
validate_message_delivered_record (WyreboxDeliveryReplayValidator *self,
    WyreboxJournalRecord *record, GError **error)
{
    g_autoptr (GError) local_error = NULL;
    g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };

    if (!wyrebox_message_delivered_payload_decode (record->payload,
        &decoded, &local_error)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "failed to decode MessageDelivered journal record at sequence %"
            G_GUINT64_FORMAT ": %s",
            record->sequence,
            local_error != NULL ? local_error->message : "unknown error");
        return FALSE;
    }

    switch (wyrebox_delivery_object_check (self->object_store,
        decoded.object_key, decoded.size_bytes, &local_error)) {
    case WYREBOX_DELIVERY_OBJECT_CHECK_OK:
        return TRUE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_MISSING:
        g_set_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
            WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_MISSING_OBJECT,
            "MessageDelivered journal record at sequence %" G_GUINT64_FORMAT
            " references unavailable raw object %s: %s",
            record->sequence, decoded.object_key, local_error->message);
        return FALSE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_SIZE_MISMATCH:
        g_set_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
            WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_SIZE_MISMATCH,
            "MessageDelivered journal record at sequence %" G_GUINT64_FORMAT
            ": %s", record->sequence, local_error->message);
        return FALSE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_HASH_MISMATCH:
        g_set_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
            WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_HASH_MISMATCH,
            "MessageDelivered journal record at sequence %" G_GUINT64_FORMAT
            " references raw object %s with SHA-256 mismatch",
            record->sequence, decoded.object_key);
        return FALSE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_INVALID_KEY:
        g_set_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
            WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_INVALID_RECORD,
            "MessageDelivered journal record at sequence %" G_GUINT64_FORMAT
            " references invalid raw object key %s: %s",
            record->sequence, decoded.object_key, local_error->message);
        return FALSE;
    case WYREBOX_DELIVERY_OBJECT_CHECK_UNREADABLE:
    default:
        g_set_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
            WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_OBJECT_UNREADABLE,
            "MessageDelivered journal record at sequence %" G_GUINT64_FORMAT
            " failed to read raw object %s: %s",
            record->sequence, decoded.object_key,
            local_error != NULL ? local_error->message : "unknown error");
        return FALSE;
    }
}

static gboolean
validate_records (WyreboxDeliveryReplayValidator *self,
    WyreboxDeliveryReplayValidatorReport *report, GError **error)
{
    g_auto (WyreboxJournalRecord) record = { 0 };
    gboolean eof = FALSE;

    while (TRUE) {
        g_autoptr (GError) record_error = NULL;

        if (!wyrebox_journal_reader_read_next (self->journal_reader,
            &record, &eof, error)) {
            if (eof)
                return TRUE;

            return FALSE;
        }

        if (record.event_type != WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED ||
            validate_message_delivered_record (self, &record, &record_error))
            continue;

        if (report == NULL || !is_counted_object_failure (record_error)) {
            g_propagate_error (error, g_steal_pointer (&record_error));
            return FALSE;
        }

        if (report->object_failure_count == 0) {
            report->first_object_failure_offset = record.offset;
            report->first_object_failure_sequence = record.sequence;
            report->first_object_failure_code = record_error->code;
        }
        report->object_failure_count++;
    }
}

gboolean
wyrebox_delivery_replay_validator_validate_all (WyreboxDeliveryReplayValidator
    *self, GError **error)
{
    g_return_val_if_fail (WYREBOX_IS_DELIVERY_REPLAY_VALIDATOR (self), FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    return validate_records (self, NULL, error);
}

gboolean
wyrebox_delivery_replay_validator_validate_all_report (
    WyreboxDeliveryReplayValidator *self,
    WyreboxDeliveryReplayValidatorReport *out_report, GError **error)
{
    g_return_val_if_fail (WYREBOX_IS_DELIVERY_REPLAY_VALIDATOR (self), FALSE);
    g_return_val_if_fail (out_report != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    *out_report = (WyreboxDeliveryReplayValidatorReport) {
        .first_object_failure_code =
            WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_INVALID_RECORD,
    };

    return validate_records (self, out_report, error);
}
