#include "wyrebox-delivery-replay-validator.h"

#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>

#include <string.h>

#define WYREBOX_SHA256_OBJECT_KEY_PREFIX_LEN 7

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
    (WyreboxDeliveryReplayValidatorClass * klass)
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

  {
    g_autoptr (GBytes) object_bytes =
        wyrebox_local_object_store_get_bytes (self->object_store,
        decoded.object_key, &local_error);
    gsize object_size = 0;
    const guint8 *object_data = NULL;
    g_autoptr (GChecksum) checksum = NULL;
    const char *actual = NULL;

    if (object_bytes == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "MessageDelivered journal record at sequence %"
          G_GUINT64_FORMAT
          " references unavailable raw object %s: %s",
          record->sequence,
          decoded.object_key,
          local_error != NULL ? local_error->message : "unknown error");
      return FALSE;
    }

    object_data = g_bytes_get_data (object_bytes, &object_size);
    if (object_size != decoded.size_bytes) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "MessageDelivered journal record at sequence %"
          G_GUINT64_FORMAT
          " references raw object %s with mismatched size: expected "
          "%" G_GUINT64_FORMAT ", got %" G_GSIZE_FORMAT,
          record->sequence,
          decoded.object_key, decoded.size_bytes, object_size);
      return FALSE;
    }

    checksum = g_checksum_new (G_CHECKSUM_SHA256);
    g_checksum_update (checksum, object_data, object_size);
    actual = g_checksum_get_string (checksum);
    if (g_strcmp0 (actual,
            decoded.object_key + WYREBOX_SHA256_OBJECT_KEY_PREFIX_LEN) != 0) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "MessageDelivered journal record at sequence %"
          G_GUINT64_FORMAT
          " references raw object %s with SHA-256 mismatch",
          record->sequence, decoded.object_key);
      return FALSE;
    }
  }

  return TRUE;
}

gboolean
wyrebox_delivery_replay_validator_validate_all (WyreboxDeliveryReplayValidator
    *self, GError **error)
{
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  g_return_val_if_fail (WYREBOX_IS_DELIVERY_REPLAY_VALIDATOR (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  while (TRUE) {
    if (!wyrebox_journal_reader_read_next (self->journal_reader,
            &record, &eof, error)) {
      if (eof)
        return TRUE;

      return FALSE;
    }

    if (record.event_type == WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED &&
        !validate_message_delivered_record (self, &record, error))
      return FALSE;
  }
}
