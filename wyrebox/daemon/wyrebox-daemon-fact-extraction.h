#pragma once

#include "wyrebox-fact-extraction-rules.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_FACT_EXTRACTION \
  (wyrebox_daemon_fact_extraction_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonFactExtraction,
    wyrebox_daemon_fact_extraction,
    WYREBOX,
    DAEMON_FACT_EXTRACTION,
    GObject)

/*
 * Extracts derived facts from delivered messages inside wyreboxd and journals
 * them as FactsExtracted records. It reads the catalog and raw objects but
 * never writes either; catch-up materializes the journaled records.
 *
 * @catalog_path: DuckDB catalog, already prepared to the current schema.
 * @rules: (transfer none): extraction rules; a reference is kept.
 * @object_store: (transfer none): raw object store; a reference is kept.
 * @journal_writer: (transfer none): live journal writer; a reference is kept.
 *
 * Returns: (transfer full): new service, or NULL with @error set.
 */
WyreboxDaemonFactExtraction *wyrebox_daemon_fact_extraction_new (
    const char *catalog_path,
    WyreboxFactExtractionRules *rules,
    WyreboxLocalObjectStore *object_store,
    WyreboxJournalWriter *journal_writer,
    GError **error);

/*
 * Appends one FactsExtracted record, in journal order, for every materialized
 * message of @account_id that has no materialized extraction yet, and stores
 * in @out_appended how many records it appended, including on failure.
 *
 * Each record carries the header facts and matching rule facts extracted from
 * the message's raw object, whose header values are first made valid UTF-8.
 * A message whose record is journaled but not yet materialized is extracted
 * again; materialization keeps only its first record.
 *
 * Fails when a raw object cannot be read or parsed, or the journal append
 * fails; records appended before the failure stay journaled.
 */
gboolean wyrebox_daemon_fact_extraction_extract_account (
    WyreboxDaemonFactExtraction *self,
    const char *account_id,
    guint *out_appended,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
