#pragma once

#include <glib-object.h>

/*
 * One EXPUNGE of ordinary-mailbox UIDs, journaled as MembershipsExpunged.
 *
 * @uids: strictly ascending UIDs to tombstone; at least one entry, each >= 1.
 *   Already-absent UIDs are omitted before append; an empty request is never
 *   journaled.
 *
 * Strings and @uids are owned by the payload and released by clear().
 */
typedef struct
{
    char *account_id;
    char *mailbox_id;
    guint64 uidvalidity;
    guint64 *uids;
    gsize n_uids;
} WyreboxMembershipsExpungedPayload;

/* *INDENT-OFF* */
G_BEGIN_DECLS

void wyrebox_memberships_expunged_payload_clear (
    WyreboxMembershipsExpungedPayload *payload);

/*
 * Returns: (transfer full): encoded payload, or NULL with @error set to
 *   G_IO_ERROR_INVALID_ARGUMENT when @payload is invalid.
 */
GBytes *wyrebox_memberships_expunged_payload_encode (
    const WyreboxMembershipsExpungedPayload *payload,
    GError **error);

/*
 * Decodes @bytes into @out_payload, which must be zero-initialized or
 * cleared. Malformed or invalid payloads fail with G_IO_ERROR_INVALID_DATA
 * and leave @out_payload untouched.
 */
gboolean wyrebox_memberships_expunged_payload_decode (GBytes *bytes,
    WyreboxMembershipsExpungedPayload *out_payload,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxMembershipsExpungedPayload,
    wyrebox_memberships_expunged_payload_clear)

G_END_DECLS
/* *INDENT-ON* */
