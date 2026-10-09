#pragma once

#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-flag-keyword-update-service.h"
#include "wyrebox-journal-writer.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Creates a flag/keyword update service backed by the mutation journal.
 *
 * Each update is checked against the materialized catalog at @catalog_path,
 * opened through wyrebox_duckdb_open_shared(): the mailbox must be a visible,
 * selectable ordinary mailbox of the request account with a visible
 * membership at the requested UID. The update is then appended to
 * @journal_writer as one durable FlagChanged record, and @materialization runs
 * a catch-up pass so the change reaches message_flags and message_keywords
 * before the service returns. A catch-up failure is logged and retried by
 * @materialization; the update is still reported as successful because it is
 * durable in the journal.
 *
 * A missing, hidden or unselectable mailbox, or a UID without a visible
 * membership, fails with G_IO_ERROR_NOT_FOUND, and a stale UIDVALIDITY with
 * G_IO_ERROR_EXISTS. Raw message objects are never read or written.
 *
 * @journal_writer: (transfer none): live journal writer; a reference is kept.
 * @materialization: (transfer none): catch-up service for @catalog_path; a
 *   reference is kept.
 *
 * Returns: (transfer full): the service, or NULL with @error set.
 */
WyreboxDaemonFlagKeywordUpdateService *
wyrebox_daemon_flag_keyword_update_service_new_journaled (
    const char *catalog_path,
    WyreboxJournalWriter *journal_writer,
    WyreboxDaemonDeliveryMaterialization *materialization,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
