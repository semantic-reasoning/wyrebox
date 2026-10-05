#pragma once

#include "wyrebox-delivery-materializer.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-metadata-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Replay journaled MessageDelivered records that are not covered by the
 * persisted materialization checkpoint and materialize them into the fixed
 * ordinary INBOX mailbox.
 *
 * @metadata_store: (transfer none): metadata source for the persisted
 *   materialization checkpoint.
 * @journal_reader: (transfer none): reader positioned at the beginning of the
 *   journal; this function advances it through replay.
 * @object_store: (transfer none): object store used by projection replay to
 *   verify immutable raw message objects.
 * @materializer: (transfer none): delivery materializer receiving the INBOX
 *   projection.
 * @account_id: account owning the fixed INBOX mailbox.
 */
gboolean wyrebox_delivery_catchup_materialize_inbox (
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer,
    const gchar *account_id,
    GError **error);

/*
 * An account whose INBOX could not be materialized during a catch-up pass.
 *
 * @account_id: (owned): the held account.
 * @journal_offset, @journal_sequence: the first record of the account that was
 *   not materialized in the pass; later records of the account were skipped.
 * @error: (owned): the G_IO_ERROR_INVALID_DATA error that held the account.
 */
typedef struct
{
  gchar *account_id;
  guint64 journal_offset;
  guint64 journal_sequence;
  GError *error;
} WyreboxDeliveryCatchupHold;

void wyrebox_delivery_catchup_hold_free (WyreboxDeliveryCatchupHold *hold);

/*
 * Result of an isolated catch-up pass.
 *
 * @holds: (owned) (element-type WyreboxDeliveryCatchupHold): held accounts in
 *   journal order of their first unmaterialized record; empty when every
 *   pending record was materialized.
 */
typedef struct
{
  GPtrArray *holds;
} WyreboxDeliveryCatchupReport;

void wyrebox_delivery_catchup_report_clear (
    WyreboxDeliveryCatchupReport *report);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDeliveryCatchupReport,
    wyrebox_delivery_catchup_report_clear)

/*
 * Replay journaled MessageDelivered records that are not covered by the
 * persisted materialization checkpoint and materialize each one into the INBOX
 * of the account recorded in its payload, as resolved by
 * wyrebox_delivery_materializer_apply_to_inbox_full().
 *
 * Consecutive records for the same account are applied in one materializer
 * transaction, in journal order. A run that fails with
 * G_IO_ERROR_INVALID_DATA holds its account: the run is rolled back, later
 * runs of that account are skipped, and the hold is added to @out_report.
 * Other accounts keep materializing. The checkpoint only advances over runs
 * committed before the first hold, so it never leads an unapplied record.
 *
 * Returns TRUE when the pass completed, with or without holds. Returns FALSE
 * with @error set when the pass was aborted: on a metadata, journal, or
 * projection failure, when a pending record has no account identity
 * (G_IO_ERROR_INVALID_DATA, before anything is materialized), or when
 * applying a run fails with an error other than G_IO_ERROR_INVALID_DATA.
 *
 * @out_report: (out caller-allocates): zero-initialized report, filled on
 *   success and left untouched when the pass is aborted, discarding holds
 *   found before the abort; clear it with
 *   wyrebox_delivery_catchup_report_clear().
 *
 * Other arguments follow wyrebox_delivery_catchup_materialize_inbox().
 */
gboolean wyrebox_delivery_catchup_materialize_account_inboxes_isolated (
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer,
    WyreboxDeliveryCatchupReport *out_report,
    GError **error);

/*
 * Like wyrebox_delivery_catchup_materialize_account_inboxes_isolated(), but a
 * pass that holds an account fails with a copy of the first hold's error.
 */
gboolean wyrebox_delivery_catchup_materialize_account_inboxes (
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
