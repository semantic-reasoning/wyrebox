#pragma once

#include <glib-object.h>

/*
 * One item of a COPY or MOVE: source UID to destination UID for one message.
 *
 * @src_uid: UID of the source membership under the payload's source mailbox
 *   and UIDVALIDITY; at least 1.
 * @dst_uid: destination UID reserved by the daemon; at least 1 and unique
 *   within the payload. The materializer applies it verbatim.
 * @internal_date_unix_us: internal date of the destination membership.
 * @message_id: non-empty id of the message the source membership references.
 * @system_flags: (nullable): NULL-terminated IMAP system flags, each one of
 *   \Seen, \Answered, \Flagged, \Deleted or \Draft, without duplicates.
 * @user_keywords: (nullable): NULL-terminated user keywords without
 *   duplicates; non-empty, without whitespace or control characters, and not
 *   starting with a backslash.
 *
 * Flags and keywords use the same rules as FlagChanged and describe the
 * destination membership.
 */
typedef struct
{
    guint64 src_uid;
    guint64 dst_uid;
    guint64 internal_date_unix_us;
    char *message_id;
    GStrv system_flags;
    GStrv user_keywords;
} WyreboxMembershipsCopiedItem;

/*
 * One COPY or MOVE, journaled as a single MembershipsCopied or
 * MembershipsMoved record. Both event types share this payload; the event
 * type alone selects whether source memberships are tombstoned (MOVE) or
 * left visible (COPY). Multi-UID commands are one record: the item at index
 * i is stored as journal_item i on its destination membership.
 *
 * @items: at least one item, owned by the payload.
 *
 * Binary layout (little-endian, version tag in the magic):
 *
 *   magic[8] = "WYREMCP1"
 *   source_uidvalidity  u64
 *   dest_uidvalidity    u64
 *   item_count          u32
 *   account_id          u32 length + bytes
 *   source_mailbox_id   u32 length + bytes
 *   dest_mailbox_id     u32 length + bytes
 *   item_count x {
 *     src_uid                u64
 *     dst_uid                u64
 *     internal_date_unix_us  u64
 *     message_id             u32 length + bytes
 *     flag_count             u32, then flag_count x (u32 length + bytes)
 *     keyword_count          u32, then keyword_count x (u32 length + bytes)
 *   }
 *
 * Strings and items are owned by the payload and released by clear().
 */
typedef struct
{
    char *account_id;
    char *source_mailbox_id;
    char *dest_mailbox_id;
    guint64 source_uidvalidity;
    guint64 dest_uidvalidity;
    WyreboxMembershipsCopiedItem *items;
    gsize n_items;
} WyreboxMembershipsCopiedPayload;

/* *INDENT-OFF* */
G_BEGIN_DECLS

void wyrebox_memberships_copied_payload_clear (
    WyreboxMembershipsCopiedPayload *payload);

/*
 * Returns: (transfer full): encoded payload, or NULL with @error set to
 *   G_IO_ERROR_INVALID_ARGUMENT when @payload is invalid.
 */
GBytes *wyrebox_memberships_copied_payload_encode (
    const WyreboxMembershipsCopiedPayload *payload,
    GError **error);

/*
 * Decodes @bytes into @out_payload, which must be zero-initialized or
 * cleared. Malformed or invalid payloads fail with G_IO_ERROR_INVALID_DATA
 * and leave @out_payload untouched.
 */
gboolean wyrebox_memberships_copied_payload_decode (GBytes *bytes,
    WyreboxMembershipsCopiedPayload *out_payload,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxMembershipsCopiedPayload,
    wyrebox_memberships_copied_payload_clear)

G_END_DECLS
/* *INDENT-ON* */
