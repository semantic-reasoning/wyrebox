#pragma once

#include "wyrebox-local-object-store.h"

#include <glib-object.h>

G_BEGIN_DECLS

#define WYREBOX_TYPE_EML_INGESTOR (wyrebox_eml_ingestor_get_type())

G_DECLARE_FINAL_TYPE (WyreboxEmlIngestor,
    wyrebox_eml_ingestor,
    WYREBOX,
    EML_INGESTOR,
    GObject)

typedef struct
{
  /*
   * Object key for the immutable raw RFC 5322 bytes.
   *
   * Ownership: caller owns and must free with g_free() or
   * wyrebox_eml_ingest_result_clear().
   */
  char *object_key;

  /*
   * Size of the ingested raw RFC 5322 bytes, in bytes.
   */
  guint64 size_bytes;
} WyreboxEmlIngestResult;

/*
 * Clears owned fields in @result and leaves it reusable as an empty result.
 */
void wyrebox_eml_ingest_result_clear (WyreboxEmlIngestResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxEmlIngestResult,
    wyrebox_eml_ingest_result_clear)

/*
 * @object_store: (transfer none): object store used for immutable raw bytes.
 *
 * Returns: (transfer full): a new EML ingestor holding a reference to
 * @object_store.
 */
WyreboxEmlIngestor *wyrebox_eml_ingestor_new (
    WyreboxLocalObjectStore *object_store);

/*
 * @bytes: (transfer none): raw RFC 5322 message bytes to ingest.
 * @out_result: (out): receives the object identity and raw byte size. The
 *   caller owns fields in the result and must clear it with
 *   wyrebox_eml_ingest_result_clear().
 *
 * Stores @bytes as an immutable raw object and reports its object key and size.
 * This boundary does not parse, journal, materialize, allocate UIDs, or define
 * duplicate policy beyond the deterministic object-store key behavior.
 */
gboolean wyrebox_eml_ingestor_ingest_bytes (WyreboxEmlIngestor *self,
    GBytes *bytes,
    WyreboxEmlIngestResult *out_result,
    GError **error);

G_END_DECLS
