#include "wyrebox-memberships-copied-payload.h"

#include <gio/gio.h>
#include <string.h>

#define MEMBERSHIPS_COPIED_PAYLOAD_MAGIC "WYREMCP1"
#define MEMBERSHIPS_COPIED_PAYLOAD_MAGIC_LEN 8
#define MEMBERSHIPS_COPIED_PAYLOAD_HEADER_SIZE 28

/* src_uid, dst_uid, date, message_id length, flag count, keyword count. */
#define MEMBERSHIPS_COPIED_ITEM_MIN_SIZE 36

static const char *const system_flag_names[] = {
    "\\Seen",
    "\\Answered",
    "\\Flagged",
    "\\Deleted",
    "\\Draft",
};

static inline void
write_u32_le (guint8 *dst, guint32 value)
{
    dst[0] = (guint8)((value >> 0) & 0xff);
    dst[1] = (guint8)((value >> 8) & 0xff);
    dst[2] = (guint8)((value >> 16) & 0xff);
    dst[3] = (guint8)((value >> 24) & 0xff);
}

static inline void
write_u64_le (guint8 *dst, guint64 value)
{
    for (guint i = 0; i < 8; i++)
        dst[i] = (guint8)((value >> (8 * i)) & 0xff);
}

static inline guint32
read_u32_le (const guint8 *buffer)
{
    return (guint32)buffer[0] |
           ((guint32)buffer[1] << 8) |
           ((guint32)buffer[2] << 16) | ((guint32)buffer[3] << 24);
}

static inline guint64
read_u64_le (const guint8 *buffer)
{
    guint64 value = 0;

    for (guint i = 0; i < 8; i++)
        value |= (guint64)buffer[i] << (8 * i);
    return value;
}

static gboolean
is_system_flag (const char *value)
{
    for (gsize i = 0; i < G_N_ELEMENTS (system_flag_names); i++) {
        if (g_strcmp0 (value, system_flag_names[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

static gboolean
is_user_keyword (const char *value)
{
    if (value == NULL || value[0] == '\0' || value[0] == '\\')
        return FALSE;

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (g_ascii_iscntrl (*cursor) || g_ascii_isspace (*cursor))
            return FALSE;
    }

    return TRUE;
}

static gboolean
validate_required_string (const char *field_name, const char *value,
    GIOErrorEnum code, GError **error)
{
    if (value == NULL || value[0] == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsCopied payload %s is required", field_name);
        return FALSE;
    }

    return TRUE;
}

static gboolean
validate_names (const char *field_name, char **values,
    gboolean (*is_valid) (const char *value), GIOErrorEnum code,
    GError **error)
{
    for (gsize i = 0; values != NULL && values[i] != NULL; i++) {
        if (!is_valid (values[i])) {
            g_set_error (error,
                G_IO_ERROR,
                code, "MembershipsCopied payload %s contains an invalid value",
                field_name);
            return FALSE;
        }

        for (gsize j = 0; j < i; j++) {
            if (strcmp (values[i], values[j]) == 0) {
                g_set_error (error,
                    G_IO_ERROR,
                    code,
                    "MembershipsCopied payload %s contains duplicate values",
                    field_name);
                return FALSE;
            }
        }
    }

    return TRUE;
}

static gsize
strv_count (char **values)
{
    return values != NULL ? g_strv_length (values) : 0;
}

static gboolean
validate_item (const WyreboxMembershipsCopiedItem *item, GIOErrorEnum code,
    GError **error)
{
    if (item->src_uid == 0 || item->dst_uid == 0) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsCopied payload uid must be at least 1");
        return FALSE;
    }

    return validate_required_string ("message_id", item->message_id, code,
               error) &&
           validate_names ("system_flags", item->system_flags, is_system_flag,
               code, error) &&
           validate_names ("user_keywords", item->user_keywords,
               is_user_keyword, code, error);
}

static gboolean
validate_payload (const WyreboxMembershipsCopiedPayload *payload,
    GIOErrorEnum code, GError **error)
{
    g_autoptr (GHashTable) dst_uids = NULL;

    if (payload == NULL) {
        g_set_error (error,
            G_IO_ERROR, code, "MembershipsCopied payload is required");
        return FALSE;
    }

    if (!validate_required_string ("account_id", payload->account_id, code,
        error) ||
        !validate_required_string ("source_mailbox_id",
        payload->source_mailbox_id, code, error) ||
        !validate_required_string ("dest_mailbox_id",
        payload->dest_mailbox_id, code, error))
        return FALSE;

    if (payload->source_uidvalidity == 0 || payload->dest_uidvalidity == 0) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsCopied payload uidvalidity is required");
        return FALSE;
    }

    if (payload->n_items == 0 || payload->items == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsCopied payload requires at least one item");
        return FALSE;
    }

    if (payload->n_items > G_MAXUINT32) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsCopied payload has too many items");
        return FALSE;
    }

    dst_uids = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
            NULL);
    for (gsize i = 0; i < payload->n_items; i++) {
        guint64 *key = NULL;

        if (!validate_item (&payload->items[i], code, error))
            return FALSE;

        key = g_new (guint64, 1);
        *key = payload->items[i].dst_uid;
        if (!g_hash_table_add (dst_uids, key)) {
            g_set_error (error,
                G_IO_ERROR,
                code, "MembershipsCopied payload destination uids must be "
                "unique");
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
append_string (GByteArray *buffer, const char *value, GError **error)
{
    guint8 prefix[sizeof (guint32)];
    gsize len = strlen (value);

    if (len > G_MAXUINT32) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "MembershipsCopied payload string is too large");
        return FALSE;
    }

    write_u32_le (prefix, (guint32)len);
    g_byte_array_append (buffer, prefix, sizeof (prefix));
    g_byte_array_append (buffer, (const guint8 *)value, len);
    return TRUE;
}

static void
append_u32 (GByteArray *buffer, guint32 value)
{
    guint8 bytes[sizeof (guint32)];

    write_u32_le (bytes, value);
    g_byte_array_append (buffer, bytes, sizeof (bytes));
}

static void
append_u64 (GByteArray *buffer, guint64 value)
{
    guint8 bytes[sizeof (guint64)];

    write_u64_le (bytes, value);
    g_byte_array_append (buffer, bytes, sizeof (bytes));
}

static gboolean
append_strv (GByteArray *buffer, char **values, GError **error)
{
    gsize count = strv_count (values);

    if (count > G_MAXUINT32) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "MembershipsCopied payload has too many flags or keywords");
        return FALSE;
    }

    append_u32 (buffer, (guint32)count);
    for (gsize i = 0; i < count; i++) {
        if (!append_string (buffer, values[i], error))
            return FALSE;
    }

    return TRUE;
}

static gboolean
read_u32 (const guint8 *data, gsize size, gsize *offset, guint32 *out_value,
    GError **error)
{
    if (size - *offset < sizeof (guint32)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "MembershipsCopied payload is truncated");
        return FALSE;
    }

    *out_value = read_u32_le (data + *offset);
    *offset += sizeof (guint32);
    return TRUE;
}

static gboolean
read_u64 (const guint8 *data, gsize size, gsize *offset, guint64 *out_value,
    GError **error)
{
    if (size - *offset < sizeof (guint64)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "MembershipsCopied payload is truncated");
        return FALSE;
    }

    *out_value = read_u64_le (data + *offset);
    *offset += sizeof (guint64);
    return TRUE;
}

static gboolean
read_string (const guint8 *data, gsize size, gsize *offset, char **out_value,
    GError **error)
{
    guint32 len = 0;

    *out_value = NULL;

    if (!read_u32 (data, size, offset, &len, error))
        return FALSE;

    if ((gsize)len > size - *offset) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "MembershipsCopied payload is truncated");
        return FALSE;
    }

    if (memchr (data + *offset, '\0', len) != NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MembershipsCopied payload string contains embedded NUL");
        return FALSE;
    }

    *out_value = g_strndup ((const char *)data + *offset, len);
    *offset += len;
    return TRUE;
}

static gboolean
read_strv (const guint8 *data, gsize size, gsize *offset, GStrv *out_values,
    GError **error)
{
    g_autoptr (GPtrArray) values = NULL;
    guint32 count = 0;

    *out_values = NULL;
    if (!read_u32 (data, size, offset, &count, error))
        return FALSE;

    if (count == 0)
        return TRUE;

    /* Every encoded string takes at least its length prefix. */
    if ((gsize)count > (size - *offset) / sizeof (guint32)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "MembershipsCopied payload is truncated");
        return FALSE;
    }

    values = g_ptr_array_new_with_free_func (g_free);
    for (guint32 i = 0; i < count; i++) {
        char *value = NULL;

        if (!read_string (data, size, offset, &value, error))
            return FALSE;
        g_ptr_array_add (values, value);
    }

    g_ptr_array_add (values, NULL);
    *out_values = (GStrv)g_ptr_array_free (g_steal_pointer (&values), FALSE);
    return TRUE;
}

static void
item_clear (WyreboxMembershipsCopiedItem *item)
{
    g_clear_pointer (&item->message_id, g_free);
    g_clear_pointer (&item->system_flags, g_strfreev);
    g_clear_pointer (&item->user_keywords, g_strfreev);
    item->src_uid = 0;
    item->dst_uid = 0;
    item->internal_date_unix_us = 0;
}

void
wyrebox_memberships_copied_payload_clear (WyreboxMembershipsCopiedPayload
    *payload)
{
    if (payload == NULL)
        return;

    for (gsize i = 0; payload->items != NULL && i < payload->n_items; i++)
        item_clear (&payload->items[i]);

    g_clear_pointer (&payload->account_id, g_free);
    g_clear_pointer (&payload->source_mailbox_id, g_free);
    g_clear_pointer (&payload->dest_mailbox_id, g_free);
    g_clear_pointer (&payload->items, g_free);
    payload->source_uidvalidity = 0;
    payload->dest_uidvalidity = 0;
    payload->n_items = 0;
}

GBytes *
wyrebox_memberships_copied_payload_encode (const
    WyreboxMembershipsCopiedPayload *payload, GError **error)
{
    g_autoptr (GByteArray) buffer = NULL;

    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (!validate_payload (payload, G_IO_ERROR_INVALID_ARGUMENT, error))
        return NULL;

    buffer = g_byte_array_new ();
    g_byte_array_append (buffer,
        (const guint8 *)MEMBERSHIPS_COPIED_PAYLOAD_MAGIC,
        MEMBERSHIPS_COPIED_PAYLOAD_MAGIC_LEN);
    append_u64 (buffer, payload->source_uidvalidity);
    append_u64 (buffer, payload->dest_uidvalidity);
    append_u32 (buffer, (guint32)payload->n_items);
    g_assert (buffer->len == MEMBERSHIPS_COPIED_PAYLOAD_HEADER_SIZE);

    if (!append_string (buffer, payload->account_id, error) ||
        !append_string (buffer, payload->source_mailbox_id, error) ||
        !append_string (buffer, payload->dest_mailbox_id, error))
        return NULL;

    for (gsize i = 0; i < payload->n_items; i++) {
        const WyreboxMembershipsCopiedItem *item = &payload->items[i];

        append_u64 (buffer, item->src_uid);
        append_u64 (buffer, item->dst_uid);
        append_u64 (buffer, item->internal_date_unix_us);
        if (!append_string (buffer, item->message_id, error) ||
            !append_strv (buffer, item->system_flags, error) ||
            !append_strv (buffer, item->user_keywords, error))
            return NULL;
    }

    return g_byte_array_free_to_bytes (g_steal_pointer (&buffer));
}

static gboolean
read_item (const guint8 *data, gsize size, gsize *offset,
    WyreboxMembershipsCopiedItem *out_item, GError **error)
{
    return read_u64 (data, size, offset, &out_item->src_uid, error) &&
           read_u64 (data, size, offset, &out_item->dst_uid, error) &&
           read_u64 (data, size, offset, &out_item->internal_date_unix_us,
               error) &&
           read_string (data, size, offset, &out_item->message_id, error) &&
           read_strv (data, size, offset, &out_item->system_flags, error) &&
           read_strv (data, size, offset, &out_item->user_keywords, error);
}

gboolean
wyrebox_memberships_copied_payload_decode (GBytes *bytes,
    WyreboxMembershipsCopiedPayload *out_payload, GError **error)
{
    const guint8 *data = NULL;
    gsize size = 0;
    gsize offset = MEMBERSHIPS_COPIED_PAYLOAD_HEADER_SIZE;
    guint32 n_items = 0;
    g_auto (WyreboxMembershipsCopiedPayload) decoded = { 0 };

    g_return_val_if_fail (bytes != NULL, FALSE);
    g_return_val_if_fail (out_payload != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    data = g_bytes_get_data (bytes, &size);
    if (size < MEMBERSHIPS_COPIED_PAYLOAD_HEADER_SIZE) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "MembershipsCopied payload is truncated");
        return FALSE;
    }

    if (memcmp (data, MEMBERSHIPS_COPIED_PAYLOAD_MAGIC,
        MEMBERSHIPS_COPIED_PAYLOAD_MAGIC_LEN) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "invalid MembershipsCopied payload magic");
        return FALSE;
    }

    decoded.source_uidvalidity = read_u64_le (data + 8);
    decoded.dest_uidvalidity = read_u64_le (data + 16);
    n_items = read_u32_le (data + 24);

    if (!read_string (data, size, &offset, &decoded.account_id, error) ||
        !read_string (data, size, &offset, &decoded.source_mailbox_id,
        error) ||
        !read_string (data, size, &offset, &decoded.dest_mailbox_id, error))
        return FALSE;

    if (n_items == 0 ||
        (gsize)n_items > (size - offset) / MEMBERSHIPS_COPIED_ITEM_MIN_SIZE) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "MembershipsCopied payload is truncated");
        return FALSE;
    }

    decoded.items = g_new0 (WyreboxMembershipsCopiedItem, n_items);
    decoded.n_items = n_items;
    for (guint32 i = 0; i < n_items; i++) {
        if (!read_item (data, size, &offset, &decoded.items[i], error))
            return FALSE;
    }

    if (offset != size) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MembershipsCopied payload length is malformed");
        return FALSE;
    }

    if (!validate_payload (&decoded, G_IO_ERROR_INVALID_DATA, error))
        return FALSE;

    *out_payload = decoded;
    memset (&decoded, 0, sizeof (decoded));
    return TRUE;
}
