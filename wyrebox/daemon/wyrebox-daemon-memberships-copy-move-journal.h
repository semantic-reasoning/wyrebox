#pragma once

#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-memberships-copy-move-service.h"
#include "wyrebox-journal-writer.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Creates a memberships copy/move service backed by the mutation journal.
 *
 * Under the daemon mutation route lock, each call runs a catch-up pass, then
 * validates source and destination mailboxes and reserves destination UIDs in
 * DuckDB under a service connection lock, appends one MembershipsCopied or
 * MembershipsMoved record, and schedules another catch-up pass. Invisible
 * source UIDs fail with not found without journaling.
 */
WyreboxDaemonMembershipsCopyMoveService *
wyrebox_daemon_memberships_copy_move_service_new_journaled (
    const char *catalog_path,
    WyreboxJournalWriter *journal_writer,
    WyreboxDaemonDeliveryMaterialization *materialization,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
