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

    if (!wyrebox_journal_reader_scan_safe_prefix (journal_reader, &prefix,
        error))
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

/*
 * Replays the records after @resume_after, or after the persisted checkpoint
 * when @resume_after is NULL or not present. @out_from_checkpoint tells
 * whether the replay started at the persisted checkpoint.
 */
static gboolean
replay_deliveries_after (WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    const WyreboxDeliveryCatchupCursor *resume_after,
    WyreboxDeliveryProjectionList *out_list, gboolean *out_from_checkpoint,
    GError **error)
{
    g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
    g_autoptr (WyreboxDeliveryProjection) projection = NULL;
    WyreboxDeliveryCatchupCursor start = { 0 };

    if (!wyrebox_schema_metadata_store_load (metadata_store, &metadata, error))
        return FALSE;

    if (!fail_if_journal_has_unsafe_suffix (journal_reader, error))
        return FALSE;

    start.present = metadata.materialization_checkpoint_present;
    start.journal_offset = metadata.materialization_checkpoint_journal_offset;
    start.journal_sequence = metadata.materialization_checkpoint_sequence;
    if (resume_after != NULL && resume_after->present) {
        *out_from_checkpoint = start.present &&
            start.journal_offset == resume_after->journal_offset &&
            start.journal_sequence == resume_after->journal_sequence;
        start = *resume_after;
    } else {
        *out_from_checkpoint = TRUE;
    }

    if (start.present &&
        !wyrebox_journal_reader_seek_after_checkpoint (journal_reader,
        start.journal_offset, start.journal_sequence, error)) {
        if (!*out_from_checkpoint)
            g_prefix_error (error, "scan cursor: ");
        return FALSE;
    }

    projection = wyrebox_delivery_projection_new (journal_reader, object_store);
    if (projection == NULL)
        return FALSE;

    return wyrebox_delivery_projection_replay_all (projection, out_list, error);
}

gboolean
wyrebox_delivery_catchup_materialize_inbox (WyreboxSchemaMetadataStore
    *metadata_store, WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer, const gchar *account_id,
    GError **error)
{
    g_auto (WyreboxDeliveryProjectionList) list = { 0 };
    gboolean from_checkpoint = FALSE;

    g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (metadata_store),
        FALSE);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (journal_reader), FALSE);
    g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), FALSE);
    g_return_val_if_fail (WYREBOX_IS_DELIVERY_MATERIALIZER (materializer),
        FALSE);
    g_return_val_if_fail (account_id != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!replay_deliveries_after (metadata_store, journal_reader,
        object_store, NULL, &list, &from_checkpoint, error))
        return FALSE;

    return wyrebox_delivery_materializer_apply_to_mailbox (materializer,
               account_id, "mailbox-inbox", "INBOX", &list, error);
}

static gboolean
fail_if_any_record_lacks_account (const WyreboxDeliveryProjectionList *list,
    GError **error)
{
    for (guint i = 0; i < list->records->len; i++) {
        const WyreboxDeliveryProjectionRecord *record =
            g_ptr_array_index (list->records, i);

        if (record->account_identity == NULL ||
            record->account_identity[0] == '\0') {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "journaled delivery at offset %" G_GUINT64_FORMAT
                ", sequence %" G_GUINT64_FORMAT " has no account identity",
                record->journal_offset, record->journal_sequence);
            return FALSE;
        }
    }

    return TRUE;
}

void
wyrebox_delivery_catchup_hold_free (WyreboxDeliveryCatchupHold *hold)
{
    if (hold == NULL)
        return;

    g_free (hold->account_id);
    g_clear_error (&hold->error);
    g_free (hold);
}

WyreboxDeliveryCatchupHold *
wyrebox_delivery_catchup_hold_copy (const WyreboxDeliveryCatchupHold *hold)
{
    WyreboxDeliveryCatchupHold *copy = NULL;

    g_return_val_if_fail (hold != NULL, NULL);

    copy = g_new0 (WyreboxDeliveryCatchupHold, 1);
    copy->account_id = g_strdup (hold->account_id);
    copy->journal_offset = hold->journal_offset;
    copy->journal_sequence = hold->journal_sequence;
    copy->error = hold->error != NULL ? g_error_copy (hold->error) : NULL;

    return copy;
}

void
wyrebox_delivery_catchup_report_clear (WyreboxDeliveryCatchupReport *report)
{
    if (report == NULL)
        return;

    g_clear_pointer (&report->holds, g_ptr_array_unref);
}

static gboolean
is_account_held (const GPtrArray *holds, const gchar *account_id)
{
    for (guint i = 0; i < holds->len; i++) {
        const WyreboxDeliveryCatchupHold *hold = g_ptr_array_index (holds, i);

        if (g_strcmp0 (hold->account_id, account_id) == 0)
            return TRUE;
    }

    return FALSE;
}

static void
add_hold (GPtrArray *holds, const WyreboxDeliveryProjectionRecord *first,
    GError *error)
{
    WyreboxDeliveryCatchupHold *hold = g_new0 (WyreboxDeliveryCatchupHold, 1);

    hold->account_id = g_strdup (first->account_identity);
    hold->journal_offset = first->journal_offset;
    hold->journal_sequence = first->journal_sequence;
    hold->error = error;
    g_ptr_array_add (holds, hold);
}

gboolean
wyrebox_delivery_catchup_materialize_account_inboxes_resumed (
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer,
    const WyreboxDeliveryCatchupCursor *resume_after,
    const GPtrArray *prior_holds, WyreboxDeliveryCatchupReport *out_report,
    GError **error)
{
    g_auto (WyreboxDeliveryProjectionList) list = { 0 };
    g_autoptr (GPtrArray) holds = NULL;
    WyreboxDeliveryCatchupCursor scanned_through = { 0 };
    gboolean from_checkpoint = FALSE;
    guint run_start = 0;

    g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (metadata_store),
        FALSE);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (journal_reader), FALSE);
    g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), FALSE);
    g_return_val_if_fail (WYREBOX_IS_DELIVERY_MATERIALIZER (materializer),
        FALSE);
    g_return_val_if_fail (out_report != NULL && out_report->holds == NULL,
        FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!replay_deliveries_after (metadata_store, journal_reader,
        object_store, resume_after, &list, &from_checkpoint, error) ||
        !fail_if_any_record_lacks_account (&list, error))
        return FALSE;

    holds = g_ptr_array_new_with_free_func (
        (GDestroyNotify)wyrebox_delivery_catchup_hold_free);
    for (guint i = 0; prior_holds != NULL && i < prior_holds->len; i++) {
        g_ptr_array_add (holds,
            wyrebox_delivery_catchup_hold_copy (g_ptr_array_index (prior_holds,
            i)));
    }

    if (list.records->len > 0) {
        const WyreboxDeliveryProjectionRecord *last =
            g_ptr_array_index (list.records, list.records->len - 1);

        scanned_through.present = TRUE;
        scanned_through.journal_offset = last->journal_offset;
        scanned_through.journal_sequence = last->journal_sequence;
    } else if (resume_after != NULL) {
        scanned_through = *resume_after;
    }

    while (run_start < list.records->len) {
        const WyreboxDeliveryProjectionRecord *first =
            g_ptr_array_index (list.records, run_start);
        g_auto (WyreboxDeliveryProjectionList) run = { 0 };
        g_autoptr (GError) run_error = NULL;
        guint run_end = run_start;

        run.records = g_ptr_array_new ();
        while (run_end < list.records->len) {
            WyreboxDeliveryProjectionRecord *record =
                g_ptr_array_index (list.records, run_end);

            if (g_strcmp0 (record->account_identity,
                first->account_identity) != 0)
                break;
            g_ptr_array_add (run.records, record);
            run_end++;
        }
        run_start = run_end;

        if (is_account_held (holds, first->account_identity))
            continue;

        if (wyrebox_delivery_materializer_apply_to_inbox_full (materializer,
            first->account_identity, &run,
            from_checkpoint && holds->len == 0, &run_error))
            continue;

        if (!g_error_matches (run_error, G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA)) {
            g_propagate_error (error, g_steal_pointer (&run_error));
            return FALSE;
        }

        add_hold (holds, first, g_steal_pointer (&run_error));
    }

    out_report->holds = g_steal_pointer (&holds);
    out_report->records_scanned = list.records->len;
    out_report->scanned_through = scanned_through;
    return TRUE;
}

gboolean
wyrebox_delivery_catchup_materialize_account_inboxes_isolated (
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer,
    WyreboxDeliveryCatchupReport *out_report, GError **error)
{
    return wyrebox_delivery_catchup_materialize_account_inboxes_resumed (
        metadata_store, journal_reader, object_store, materializer, NULL, NULL,
        out_report, error);
}

gboolean
wyrebox_delivery_catchup_materialize_account_inboxes (
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer, GError **error)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    const WyreboxDeliveryCatchupHold *hold = NULL;

    if (!wyrebox_delivery_catchup_materialize_account_inboxes_isolated (
            metadata_store, journal_reader, object_store, materializer,
            &report, error))
        return FALSE;

    if (report.holds->len == 0)
        return TRUE;

    hold = g_ptr_array_index (report.holds, 0);
    g_propagate_error (error, g_error_copy (hold->error));
    return FALSE;
}
