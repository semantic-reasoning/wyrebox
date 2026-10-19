#pragma once

#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-message-append-service.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Creates a message append service backed by the mutation journal.
 *
 * Under the daemon mutation route lock, each call runs a catch-up pass, then
 * validates the destination mailbox and reserves a UID in DuckDB under a
 * service connection lock, durably stores the raw object under the journal
 * append guard, appends one MessageAppended record, and schedules another
 * catch-up pass.
 */
WyreboxDaemonMessageAppendService *
wyrebox_daemon_message_append_service_new_journaled (
    const char *catalog_path,
    WyreboxJournalWriter *journal_writer,
    WyreboxLocalObjectStore *object_store,
    WyreboxDaemonDeliveryMaterialization *materialization,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
