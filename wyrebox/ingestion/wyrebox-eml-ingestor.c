#include "wyrebox-eml-ingestor.h"

#include "wyrebox-eml-metadata.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>

struct _WyreboxEmlIngestor
{
  GObject parent_instance;
  WyreboxLocalObjectStore *object_store;
  WyreboxJournalWriter *journal_writer;
};

typedef struct
{
  WyreboxEmlIngestor *ingestor;
  GBytes *bytes;
  const char *object_key;
  guint64 size_bytes;
  const WyreboxEmlMetadata *metadata;
  const char *delivery_id;
  const char *queue_id;
  const char *account_identity;
  const char *envelope_sender;
  const gchar *const *recipients;
  WyreboxEmlIngestResult *result;
} DeliveryAppendContext;

G_DEFINE_TYPE (WyreboxEmlIngestor, wyrebox_eml_ingestor, G_TYPE_OBJECT);

static void
wyrebox_eml_ingestor_finalize (GObject *object)
{
  WyreboxEmlIngestor *self = WYREBOX_EML_INGESTOR (object);

  g_clear_object (&self->object_store);
  g_clear_object (&self->journal_writer);

  G_OBJECT_CLASS (wyrebox_eml_ingestor_parent_class)->finalize (object);
}

static void
wyrebox_eml_ingestor_class_init (WyreboxEmlIngestorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_eml_ingestor_finalize;
}

static void
wyrebox_eml_ingestor_init (WyreboxEmlIngestor *self)
{
}

void
wyrebox_eml_ingest_result_clear (WyreboxEmlIngestResult *result)
{
  if (result == NULL)
    return;

  g_clear_pointer (&result->object_key, g_free);
  result->size_bytes = 0;
  result->journal_offset = 0;
  result->journal_sequence = 0;
}

static WyreboxEmlIngestor *
wyrebox_eml_ingestor_new_internal (WyreboxLocalObjectStore *object_store,
    WyreboxJournalWriter *journal_writer)
{
  g_autoptr (WyreboxEmlIngestor) self = NULL;

  g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), NULL);
  g_return_val_if_fail (journal_writer == NULL ||
      WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);

  self = g_object_new (WYREBOX_TYPE_EML_INGESTOR, NULL);
  self->object_store = g_object_ref (object_store);
  if (journal_writer != NULL)
    self->journal_writer = g_object_ref (journal_writer);

  return g_steal_pointer (&self);
}

WyreboxEmlIngestor *
wyrebox_eml_ingestor_new (WyreboxLocalObjectStore *object_store)
{
  return wyrebox_eml_ingestor_new_internal (object_store, NULL);
}

WyreboxEmlIngestor *
wyrebox_eml_ingestor_new_with_journal (WyreboxLocalObjectStore *object_store,
    WyreboxJournalWriter *journal_writer)
{
  g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);

  return wyrebox_eml_ingestor_new_internal (object_store, journal_writer);
}

gboolean
wyrebox_eml_ingestor_has_journal_writer (WyreboxEmlIngestor *self)
{
  g_return_val_if_fail (WYREBOX_IS_EML_INGESTOR (self), FALSE);

  return self->journal_writer != NULL;
}

static char *
compute_object_key (GBytes *bytes)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (bytes, &size);
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  g_checksum_update (checksum, data, size);

  return g_strdup_printf ("sha256:%s", g_checksum_get_string (checksum));
}

static const char *
normalize_queue_id (const char *queue_id)
{
  return queue_id != NULL && queue_id[0] != '\0' ? queue_id : "";
}

static gboolean
strv_equal (gchar **left, const gchar *const *right)
{
  gsize index = 0;

  if (left == NULL)
    return right == NULL || right[0] == NULL;

  if (right == NULL)
    return left[0] == NULL;

  while (left[index] != NULL && right[index] != NULL) {
    if (g_strcmp0 (left[index], right[index]) != 0)
      return FALSE;
    index++;
  }

  return left[index] == NULL && right[index] == NULL;
}

static gboolean
delivery_identity_matches (const WyreboxMessageDeliveredPayload *payload,
    const char *delivery_id, const char *queue_id)
{
  if (payload->delivery_id == NULL)
    return FALSE;

  return g_strcmp0 (payload->delivery_id, delivery_id) == 0 &&
      g_strcmp0 (normalize_queue_id (payload->queue_id),
      normalize_queue_id (queue_id)) == 0;
}

static gboolean
delivery_retry_matches (const WyreboxMessageDeliveredPayload *payload,
    const char *object_key, guint64 size_bytes, const char *account_identity,
    const char *envelope_sender, const gchar *const *recipients)
{
  return g_strcmp0 (payload->object_key, object_key) == 0 &&
      payload->size_bytes == size_bytes &&
      g_strcmp0 (payload->account_identity, account_identity) == 0 &&
      g_strcmp0 (payload->envelope_sender, envelope_sender) == 0 &&
      strv_equal (payload->recipients, recipients);
}

static gboolean
find_delivery_retry_result (const char *journal_root_dir,
    const char *object_key, guint64 size_bytes, const char *delivery_id,
    const char *queue_id, const char *account_identity,
    const char *envelope_sender, const gchar *const *recipients,
    gboolean *out_found, WyreboxEmlIngestResult *out_result, GError **error)
{
  g_autoptr (WyreboxJournalReader) reader = NULL;

  *out_found = FALSE;

  if (delivery_id == NULL)
    return TRUE;

  reader = wyrebox_journal_reader_new (journal_root_dir, error);
  if (reader == NULL)
    return FALSE;

  while (TRUE) {
    g_auto (WyreboxJournalRecord) record = { 0 };
    g_auto (WyreboxMessageDeliveredPayload) payload = { 0 };
    gboolean eof = FALSE;

    if (!wyrebox_journal_reader_read_next (reader, &record, &eof, error)) {
      if (eof)
        return TRUE;

      return FALSE;
    }

    if (record.event_type != WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED)
      continue;

    if (!wyrebox_message_delivered_payload_decode (record.payload, &payload,
            error))
      return FALSE;

    if (!delivery_identity_matches (&payload, delivery_id, queue_id))
      continue;

    if (!delivery_retry_matches (&payload, object_key, size_bytes,
            account_identity, envelope_sender, recipients)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_EXISTS,
          "delivery identity already exists with different message data");
      return FALSE;
    }

    out_result->object_key = g_strdup (payload.object_key);
    out_result->size_bytes = payload.size_bytes;
    out_result->journal_offset = record.offset;
    out_result->journal_sequence = record.sequence;
    *out_found = TRUE;
    return TRUE;
  }
}

static gboolean
append_delivery_if_absent_locked (const char *journal_root_dir,
    gpointer user_data, GBytes **out_payload, guint64 *out_offset,
    guint64 *out_sequence, GError **error)
{
  DeliveryAppendContext *context = user_data;
  g_autofree char *stored_object_key = NULL;
  g_autoptr (GBytes) payload = NULL;
  gboolean found = FALSE;

  if (!find_delivery_retry_result (journal_root_dir, context->object_key,
          context->size_bytes, context->delivery_id, context->queue_id,
          context->account_identity, context->envelope_sender,
          context->recipients, &found, context->result, error))
    return FALSE;

  if (found) {
    *out_offset = context->result->journal_offset;
    *out_sequence = context->result->journal_sequence;
    *out_payload = NULL;
    return TRUE;
  }

  if (!wyrebox_local_object_store_put_bytes (context->ingestor->object_store,
          context->bytes, &stored_object_key, error))
    return FALSE;

  payload = wyrebox_message_delivered_payload_encode_with_identity
      (stored_object_key, context->size_bytes, context->metadata,
      (guint64) g_get_real_time (), context->delivery_id, context->queue_id,
      context->account_identity, context->envelope_sender, context->recipients,
      error);
  if (payload == NULL)
    return FALSE;

  context->result->object_key = g_steal_pointer (&stored_object_key);
  context->result->size_bytes = context->size_bytes;
  *out_payload = g_steal_pointer (&payload);
  return TRUE;
}

static gboolean
wyrebox_eml_ingestor_ingest_bytes_internal (WyreboxEmlIngestor *self,
    GBytes *bytes, const char *delivery_id, const char *queue_id,
    const char *account_identity, const char *envelope_sender,
    const gchar *const *recipients, WyreboxEmlIngestResult *out_result,
    GError **error)
{
  g_autoptr (GBytes) payload = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };

  g_return_val_if_fail (WYREBOX_IS_EML_INGESTOR (self), FALSE);
  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  result.size_bytes = (guint64) g_bytes_get_size (bytes);

  if (self->journal_writer != NULL) {
    g_auto (WyreboxEmlMetadata) metadata = { 0 };

    if (!wyrebox_eml_metadata_parse_bytes (bytes, &metadata, error))
      return FALSE;

    if (!wyrebox_local_object_store_put_bytes (self->object_store, bytes,
            &result.object_key, error))
      return FALSE;

    payload = wyrebox_message_delivered_payload_encode_with_identity
        (result.object_key, result.size_bytes, &metadata,
        (guint64) g_get_real_time (), delivery_id, queue_id, account_identity,
        envelope_sender, recipients, error);
    if (payload == NULL)
      return FALSE;

    if (!wyrebox_journal_writer_append (self->journal_writer,
            WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
            payload, &result.journal_offset, &result.journal_sequence, error))
      return FALSE;
  } else {
    if (!wyrebox_local_object_store_put_bytes (self->object_store, bytes,
            &result.object_key, error))
      return FALSE;
  }

  *out_result = result;
  result.object_key = NULL;
  result.size_bytes = 0;
  result.journal_offset = 0;
  result.journal_sequence = 0;

  return TRUE;
}

gboolean
wyrebox_eml_ingestor_ingest_bytes (WyreboxEmlIngestor *self,
    GBytes *bytes, WyreboxEmlIngestResult *out_result, GError **error)
{
  return wyrebox_eml_ingestor_ingest_bytes_internal (self, bytes, NULL, NULL,
      NULL, NULL, NULL, out_result, error);
}

gboolean
wyrebox_eml_ingestor_ingest_delivery_bytes (WyreboxEmlIngestor *self,
    GBytes *bytes, const char *delivery_id, const char *queue_id,
    const char *account_identity, const char *envelope_sender,
    const gchar *const *recipients, WyreboxEmlIngestResult *out_result,
    GError **error)
{
  g_autofree char *object_key = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  g_auto (WyreboxEmlMetadata) metadata = { 0 };
  DeliveryAppendContext context = { 0 };

  g_return_val_if_fail (WYREBOX_IS_EML_INGESTOR (self), FALSE);
  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->journal_writer == NULL)
    return wyrebox_eml_ingestor_ingest_bytes_internal (self, bytes,
        delivery_id, queue_id, account_identity, envelope_sender, recipients,
        out_result, error);

  result.size_bytes = (guint64) g_bytes_get_size (bytes);
  if (!wyrebox_eml_metadata_parse_bytes (bytes, &metadata, error))
    return FALSE;

  object_key = compute_object_key (bytes);
  context.ingestor = self;
  context.bytes = bytes;
  context.object_key = object_key;
  context.size_bytes = result.size_bytes;
  context.metadata = &metadata;
  context.delivery_id = delivery_id;
  context.queue_id = queue_id;
  context.account_identity = account_identity;
  context.envelope_sender = envelope_sender;
  context.recipients = recipients;
  context.result = &result;

  if (!wyrebox_journal_writer_append_guarded (self->journal_writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          append_delivery_if_absent_locked, &context, &result.journal_offset,
          &result.journal_sequence, error))
    return FALSE;

  *out_result = result;
  result.object_key = NULL;
  result.size_bytes = 0;
  result.journal_offset = 0;
  result.journal_sequence = 0;

  return TRUE;
}
