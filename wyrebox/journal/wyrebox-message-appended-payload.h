#pragma once

#include <glib-object.h>

/*
 * One Dovecot APPEND into an ordinary mailbox, journaled as a single
 * MessageAppended record. The record is distinct from MessageDelivered: it
 * names its destination mailbox and carries the UID and UIDVALIDITY the
 * daemon reserved, plus the initial flags and keywords of the new
 * membership. There is no idempotency key.
 *
 * @account_id: non-empty account the mailbox belongs to.
 * @mailbox_id: non-empty destination mailbox id. For an account whose INBOX
 *   has never been materialized this is "inbox:<account_id>".
 * @uidvalidity: UIDVALIDITY of the destination mailbox; at least 1.
 * @uid: destination UID reserved by the daemon; at least 1. The materializer
 *   applies it verbatim.
 * @object_key: immutable raw object key, sha256:<64 lowercase hex>.
 * @size_bytes: size of the raw RFC 5322 bytes.
 * @internal_date_unix_us: internal date as Unix microseconds; zero means
 *   absent/unknown.
 * @message_id .. @date: nullable raw unfolded header values, as in
 *   MessageDelivered.
 * @duplicate_message_id_count: number of additional Message-ID headers after
 *   the first canonical value.
 * @message_id_span_*, @subject_span_*: optional raw byte spans of the first
 *   canonical Message-ID and Subject headers. Start and end are zero and
 *   ignored unless the matching *_valid flag is set, and start must not
 *   exceed end.
 * @system_flags: (nullable): NULL-terminated IMAP system flags, each one of
 *   \Seen, \Answered, \Flagged, \Deleted or \Draft, without duplicates.
 * @user_keywords: (nullable): NULL-terminated user keywords without
 *   duplicates; non-empty, without whitespace or control characters, and not
 *   starting with a backslash.
 *
 * Binary layout (little-endian, version tag in the magic):
 *
 *   magic[8] = "WYREMAP1"
 *   uidvalidity                 u64
 *   uid                         u64
 *   size_bytes                  u64
 *   internal_date_unix_us       u64
 *   duplicate_message_id_count  u32
 *   span_flags                  u32  (bit 0 message id span, bit 1 subject)
 *   message_id_span_start       u64
 *   message_id_span_end         u64
 *   subject_span_start          u64
 *   subject_span_end            u64
 *   account_id                  u32 length + bytes
 *   mailbox_id                  u32 length + bytes
 *   object_key                  u32 length + bytes
 *   message_id, subject, from, to, cc, bcc, date:
 *                               u32 length + bytes, or G_MAXUINT32 for NULL
 *   flag_count                  u32, then flag_count x (u32 length + bytes)
 *   keyword_count               u32, then keyword_count x (u32 length + bytes)
 *
 * Strings and vectors are owned by the payload and released by clear().
 */
typedef struct
{
    char *account_id;
    char *mailbox_id;
    guint64 uidvalidity;
    guint64 uid;
    char *object_key;
    guint64 size_bytes;
    guint64 internal_date_unix_us;
    char *message_id;
    char *subject;
    char *from;
    char *to;
    char *cc;
    char *bcc;
    char *date;
    guint duplicate_message_id_count;
    gboolean message_id_span_valid;
    guint64 message_id_span_start;
    guint64 message_id_span_end;
    gboolean subject_span_valid;
    guint64 subject_span_start;
    guint64 subject_span_end;
    GStrv system_flags;
    GStrv user_keywords;
} WyreboxMessageAppendedPayload;

/* *INDENT-OFF* */
G_BEGIN_DECLS

void wyrebox_message_appended_payload_clear (
    WyreboxMessageAppendedPayload *payload);

/*
 * Returns: (transfer full): encoded payload, or NULL with @error set to
 *   G_IO_ERROR_INVALID_ARGUMENT when @payload is invalid.
 */
GBytes *wyrebox_message_appended_payload_encode (
    const WyreboxMessageAppendedPayload *payload,
    GError **error);

/*
 * Decodes @bytes into @out_payload, which must be zero-initialized or
 * cleared. Malformed or invalid payloads fail with G_IO_ERROR_INVALID_DATA
 * and leave @out_payload untouched.
 */
gboolean wyrebox_message_appended_payload_decode (GBytes *bytes,
    WyreboxMessageAppendedPayload *out_payload,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxMessageAppendedPayload,
    wyrebox_message_appended_payload_clear)

G_END_DECLS
/* *INDENT-ON* */
