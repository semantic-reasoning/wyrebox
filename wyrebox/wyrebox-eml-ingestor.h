#pragma once

#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
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

  /*
   * Durable journal record location for the MessageDelivered event.
   *
   * When the ingestor has no journal writer configured, both journal_offset and
   * journal_sequence are zero.
   */
  guint64 journal_offset;
  guint64 journal_sequence;
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
 * @object_store: (transfer none): object store used for immutable raw bytes.
 * @journal_writer: (transfer none): journal writer used for durable delivery
 *   events.
 *
 * Returns: (transfer full): a new EML ingestor holding references to both
 * dependencies.
 */
WyreboxEmlIngestor *wyrebox_eml_ingestor_new_with_journal (
    WyreboxLocalObjectStore *object_store,
    WyreboxJournalWriter *journal_writer);

/*
 * @bytes: (transfer none): raw RFC 5322 message bytes to ingest.
 * @out_result: (out): receives the object identity, raw byte size, and durable
 *   journal location when a journal writer is configured. The caller owns
 *   fields in the result and must clear it with
 *   wyrebox_eml_ingest_result_clear().
 *
 * Stores @bytes as an immutable raw object and reports its object key, size,
 * and journal location. When no journal writer is configured, journal_offset
 * and journal_sequence are returned as 0. This boundary does not parse,
 * materialize, allocate UIDs, or define duplicate policy beyond the
 * deterministic object-store key behavior.
 */
gboolean wyrebox_eml_ingestor_ingest_bytes (WyreboxEmlIngestor *self,
    GBytes *bytes,
    WyreboxEmlIngestResult *out_result,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
