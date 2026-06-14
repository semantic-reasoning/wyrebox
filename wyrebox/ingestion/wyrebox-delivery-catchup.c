#include "wyrebox-delivery-catchup.h"

#include "wyrebox-delivery-projection.h"

#include <gio/gio.h>

static const gchar *
safe_prefix_stop_reason_to_string (WyreboxJournalSafePrefixStopReason reason)
{
  switch (reason) {
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EOF:
      return "eof";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_MISSING_SEGMENT:
      return "missing-segment";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EMPTY_SEGMENT:
      return "empty-segment";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_HEADER:
      return "partial-header";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_RECORD:
      return "partial-record";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_MAGIC:
      return "invalid-magic";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_HEADER_SIZE:
      return "invalid-header-size";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_VERSION:
      return "invalid-version";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SEQUENCE:
      return "invalid-sequence";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SIZE:
      return "invalid-size";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_ZERO_EVENT_TYPE_LENGTH:
      return "zero-event-type-length";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE:
      return "unknown-event-type";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_CHECKSUM_MISMATCH:
      return "checksum-mismatch";
    default:
      return "unknown";
  }
}

static gboolean
fail_if_journal_has_unsafe_suffix (WyreboxJournalReader *journal_reader,
    GError **error)
{
  WyreboxJournalSafePrefix prefix = { 0 };
  const gchar *stop_reason = NULL;

  if (!wyrebox_journal_reader_scan_safe_prefix (journal_reader, &prefix, error))
    return FALSE;

  if (!prefix.unsafe_suffix_found)
    return TRUE;

  stop_reason = safe_prefix_stop_reason_to_string (prefix.stop_reason);
  if (prefix.has_last_safe_sequence) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "journal unsafe suffix found before delivery catch-up replay: "
        "stop reason %s, unsafe offset %" G_GUINT64_FORMAT ", safe end "
        "offset %" G_GUINT64_FORMAT ", last safe sequence %"
        G_GUINT64_FORMAT ", available size %" G_GUINT64_FORMAT
        ", required size %" G_GUINT64_FORMAT,
        stop_reason, prefix.unsafe_offset, prefix.safe_end_offset,
        prefix.last_safe_sequence, prefix.unsafe_available_size,
        prefix.unsafe_required_size);
  } else {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "journal unsafe suffix found before delivery catch-up replay: "
        "stop reason %s, unsafe offset %" G_GUINT64_FORMAT ", safe end "
        "offset %" G_GUINT64_FORMAT ", last safe sequence none, "
        "available size %" G_GUINT64_FORMAT ", required size %"
        G_GUINT64_FORMAT,
        stop_reason, prefix.unsafe_offset, prefix.safe_end_offset,
        prefix.unsafe_available_size, prefix.unsafe_required_size);
  }

  return FALSE;
}

gboolean
wyrebox_delivery_catchup_materialize_inbox (WyreboxSchemaMetadataStore
    *metadata_store, WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer, const gchar *account_id,
    GError **error)
{
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (WyreboxDeliveryProjection) projection = NULL;
  g_auto (WyreboxDeliveryProjectionList) list = { 0 };

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (metadata_store),
      FALSE);
  g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (journal_reader), FALSE);
  g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), FALSE);
  g_return_val_if_fail (WYREBOX_IS_DELIVERY_MATERIALIZER (materializer), FALSE);
  g_return_val_if_fail (account_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_schema_metadata_store_load (metadata_store, &metadata, error))
    return FALSE;

  if (!fail_if_journal_has_unsafe_suffix (journal_reader, error))
    return FALSE;

  if (metadata.materialization_checkpoint_present &&
      !wyrebox_journal_reader_seek_after_checkpoint (journal_reader,
          metadata.materialization_checkpoint_journal_offset,
          metadata.materialization_checkpoint_sequence, error))
    return FALSE;

  projection = wyrebox_delivery_projection_new (journal_reader, object_store);
  if (projection == NULL)
    return FALSE;

  if (!wyrebox_delivery_projection_replay_all (projection, &list, error))
    return FALSE;

  return wyrebox_delivery_materializer_apply_to_mailbox (materializer,
      account_id, "mailbox-inbox", "INBOX", &list, error);
}
