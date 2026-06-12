#include "wyrebox-delivery-catchup.h"

#include "wyrebox-delivery-projection.h"

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
