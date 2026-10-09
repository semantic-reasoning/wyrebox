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
 * @object_store: (transfer none): object store used to verify immutable raw
 *   message objects; any raw object failure fails the replay.
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
 * @error: (owned): the error that held the account: G_IO_ERROR_INVALID_DATA,
 *   or the object store's error when a raw object could not be read.
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
 * Returns: (transfer full): a deep copy of @hold.
 */
WyreboxDeliveryCatchupHold *wyrebox_delivery_catchup_hold_copy (
    const WyreboxDeliveryCatchupHold *hold);

/*
 * A journal record position, identified by its offset and sequence.
 */
typedef struct
{
  gboolean present;
  guint64 journal_offset;
  guint64 journal_sequence;
} WyreboxDeliveryCatchupCursor;

/*
 * Result of an isolated catch-up pass.
 *
 * @holds: (owned) (element-type WyreboxDeliveryCatchupHold): held accounts in
 *   journal order of their first unmaterialized record, including holds
 *   carried into a resumed pass; empty when every pending record was
 *   materialized.
 * @records_scanned: delivery and flag change records replayed by the pass.
 * @scanned_through: the last record replayed by the pass, or the
 *   position the pass resumed after when it replayed none; not present when a
 *   pass from the checkpoint replayed nothing.
 */
typedef struct
{
  GPtrArray *holds;
  guint records_scanned;
  WyreboxDeliveryCatchupCursor scanned_through;
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
 * FlagChanged records are replayed in journal order with the deliveries. Each
 * is applied on its own with wyrebox_delivery_materializer_apply_flag_change()
 * and follows the same hold rules as a delivery run of its account: it is
 * skipped while the account is held, and a G_IO_ERROR_INVALID_DATA failure,
 * such as a target that is not materialized, holds the account.
 *
 * Consecutive deliveries for the same account are applied in one materializer
 * transaction, in journal order. Before a run is applied, the raw object of
 * each of its records is checked with
 * wyrebox_delivery_projection_check_record_object(). A run whose raw object
 * check fails for any reason, or whose apply fails with
 * G_IO_ERROR_INVALID_DATA, holds its account: nothing of the run is applied,
 * later runs of that account are skipped, and the hold is added to
 * @out_report. Other accounts keep materializing. Records sharing a
 * deduplicated raw object hold every account that references it. The
 * checkpoint only advances over runs committed before the first hold, so it
 * never leads an unapplied record.
 *
 * Returns TRUE when the pass completed, with or without holds. Returns FALSE
 * with @error set when the pass was aborted: on a metadata or journal
 * failure, when deliveries are pending but the object store root fails
 * wyrebox_local_object_store_check_root(), for example because the object
 * store is not mounted, when a pending record has no account identity
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
 * Like wyrebox_delivery_catchup_materialize_account_inboxes_isolated(), but
 * replays only the records after @resume_after and treats @prior_holds as
 * already held: their runs are skipped and they are copied into @out_report
 * ahead of new holds.
 *
 * The checkpoint advances only when @prior_holds is empty and @resume_after
 * is NULL, not present, or equal to the persisted checkpoint, so resuming past
 * records that were skipped for a hold never moves the checkpoint over them.
 * A @resume_after that no longer matches the journal aborts the pass with an
 * error prefixed "scan cursor".
 *
 * @resume_after: (nullable): a record previously reported as
 *   @scanned_through; NULL or not present replays from the checkpoint.
 * @prior_holds: (nullable) (element-type WyreboxDeliveryCatchupHold): holds
 *   from the pass that reported @resume_after; not modified.
 */
gboolean wyrebox_delivery_catchup_materialize_account_inboxes_resumed (
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer,
    const WyreboxDeliveryCatchupCursor *resume_after,
    const GPtrArray *prior_holds,
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
