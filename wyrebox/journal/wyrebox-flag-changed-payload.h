#pragma once

#include <glib-object.h>

typedef enum
{
    WYREBOX_FLAG_CHANGED_MODE_SET = 0,
    WYREBOX_FLAG_CHANGED_MODE_CLEAR = 1,
    WYREBOX_FLAG_CHANGED_MODE_REPLACE = 2,
} WyreboxFlagChangedMode;

/*
 * One flag/keyword update of a mailbox membership, journaled as a single
 * FlagChanged record.
 *
 * @system_flags: (nullable): NULL-terminated IMAP system flags, each one of
 *   \Seen, \Answered, \Flagged, \Deleted or \Draft, without duplicates.
 * @user_keywords: (nullable): NULL-terminated user keywords without
 *   duplicates; non-empty, without whitespace or control characters, and not
 *   starting with a backslash.
 *
 * SET and CLEAR require at least one system flag or keyword. REPLACE with no
 * system flags and no keywords clears both sets.
 *
 * Strings and vectors are owned by the payload and released by clear().
 */
typedef struct
{
    char *account_id;
    char *mailbox_id;
    guint64 uidvalidity;
    guint64 uid;
    WyreboxFlagChangedMode mode;
    GStrv system_flags;
    GStrv user_keywords;
} WyreboxFlagChangedPayload;

/* *INDENT-OFF* */
G_BEGIN_DECLS

void wyrebox_flag_changed_payload_clear (WyreboxFlagChangedPayload *payload);

/*
 * Returns: (transfer full): encoded payload, or NULL with @error set to
 *   G_IO_ERROR_INVALID_ARGUMENT when @payload is invalid.
 */
GBytes *wyrebox_flag_changed_payload_encode (
    const WyreboxFlagChangedPayload *payload,
    GError **error);

/*
 * Decodes @bytes into @out_payload, which must be zero-initialized or
 * cleared. Malformed or invalid payloads fail with G_IO_ERROR_INVALID_DATA
 * and leave @out_payload untouched.
 */
gboolean wyrebox_flag_changed_payload_decode (GBytes *bytes,
    WyreboxFlagChangedPayload *out_payload,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxFlagChangedPayload,
    wyrebox_flag_changed_payload_clear)

G_END_DECLS
/* *INDENT-ON* */
