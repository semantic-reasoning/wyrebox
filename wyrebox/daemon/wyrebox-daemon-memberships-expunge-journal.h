#pragma once

#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-memberships-expunge-service.h"
#include "wyrebox-journal-writer.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Creates a memberships expunge service backed by the mutation journal.
 *
 * Each expunge is checked against the materialized catalog at @catalog_path.
 * Visible ordinary-mailbox UIDs from the request are journaled as one
 * MembershipsExpunged record; already-absent UIDs are omitted. When every
 * requested UID is absent, the call succeeds without appending a journal
 * record.
 */
WyreboxDaemonMembershipsExpungeService *
wyrebox_daemon_memberships_expunge_service_new_journaled (
    const char *catalog_path,
    WyreboxJournalWriter *journal_writer,
    WyreboxDaemonDeliveryMaterialization *materialization,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
