#pragma once

#include "wyrebox-journal-reader.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_MAIL_EVENT_MAGIC "wyrebox-mail-event/1"

/*
 * Projects a journal record into the account-scoped mail event wire form.
 *
 * Only MessageDelivered, FlagChanged and DerivedViewMembershipChanged records
 * whose account equals @account_identity are part of an account's stream;
 * for any other record this returns TRUE with *@out_event set to NULL.
 *
 * The event is UTF-8 text: the magic line WYREBOX_DAEMON_MAIL_EVENT_MAGIC
 * followed by "key=value" lines, each ending in '\n'. Every event carries
 * offset, sequence, event_type and account, followed by type-specific fields:
 *
 * - MessageDelivered: delivery_id (when known), size_bytes,
 *   internal_date_unix_us.
 * - FlagChanged: mailbox_id, uidvalidity, uid, mode (set, clear or replace),
 *   system_flags and user_keywords (space-separated, possibly empty).
 * - DerivedViewMembershipChanged: view_id, uidvalidity, uid, message_id,
 *   is_visible (true or false).
 *
 * In values, '%', control characters and DEL are written as %XX. Raw
 * journal payloads are never exposed: envelope recipients, header metadata
 * and object keys stay internal.
 *
 * @record: (transfer none): the journal record to project.
 * @out_event: (out) (transfer full) (nullable): the event bytes, or NULL when
 *   the record is not part of the account's stream.
 *
 * Fails with G_IO_ERROR_INVALID_DATA when a record of a streamed type has a
 * payload that does not decode.
 */
gboolean wyrebox_daemon_mail_event_project (const WyreboxJournalRecord *record,
    const char *account_identity, GBytes **out_event, GError **error);

G_END_DECLS
/* *INDENT-ON* */
