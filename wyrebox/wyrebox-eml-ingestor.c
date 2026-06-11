#include "wyrebox-eml-ingestor.h"

struct _WyreboxEmlIngestor
{
  GObject parent_instance;
  WyreboxLocalObjectStore *object_store;
};

G_DEFINE_TYPE (WyreboxEmlIngestor, wyrebox_eml_ingestor, G_TYPE_OBJECT);

static void
wyrebox_eml_ingestor_finalize (GObject *object)
{
  WyreboxEmlIngestor *self = WYREBOX_EML_INGESTOR (object);

  g_clear_object (&self->object_store);

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
}

WyreboxEmlIngestor *
wyrebox_eml_ingestor_new (WyreboxLocalObjectStore *object_store)
{
  g_autoptr (WyreboxEmlIngestor) self = NULL;

  g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), NULL);

  self = g_object_new (WYREBOX_TYPE_EML_INGESTOR, NULL);
  self->object_store = g_object_ref (object_store);

  return g_steal_pointer (&self);
}

gboolean
wyrebox_eml_ingestor_ingest_bytes (WyreboxEmlIngestor *self,
    GBytes *bytes, WyreboxEmlIngestResult *out_result, GError **error)
{
  g_auto (WyreboxEmlIngestResult) result = { 0 };

  g_return_val_if_fail (WYREBOX_IS_EML_INGESTOR (self), FALSE);
  g_return_val_if_fail (bytes != NULL, FALSE);
  g_return_val_if_fail (out_result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_local_object_store_put_bytes (self->object_store, bytes,
          &result.object_key, error))
    return FALSE;

  result.size_bytes = (guint64) g_bytes_get_size (bytes);
  *out_result = result;
  result.object_key = NULL;
  result.size_bytes = 0;

  return TRUE;
}
