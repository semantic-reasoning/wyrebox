#include "wyrebox-flag-changed-payload.h"

#include <gio/gio.h>
#include <string.h>

#define WYREBOX_FLAG_CHANGED_PAYLOAD_MAGIC "WYREFLC1"
#define WYREBOX_FLAG_CHANGED_PAYLOAD_MAGIC_LEN 8
#define WYREBOX_FLAG_CHANGED_PAYLOAD_HEADER_SIZE 33

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
            code, "FlagChanged payload %s is required", field_name);
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
                code, "FlagChanged payload %s contains an invalid value",
                field_name);
            return FALSE;
        }

        for (gsize j = 0; j < i; j++) {
            if (strcmp (values[i], values[j]) == 0) {
                g_set_error (error,
                    G_IO_ERROR,
                    code, "FlagChanged payload %s contains duplicate values",
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
validate_payload (const WyreboxFlagChangedPayload *payload, GIOErrorEnum code,
    GError **error)
{
    if (payload == NULL) {
        g_set_error (error,
            G_IO_ERROR, code, "FlagChanged payload is required");
        return FALSE;
    }

    if (!validate_required_string ("account_id", payload->account_id, code,
        error) ||
        !validate_required_string ("mailbox_id", payload->mailbox_id, code,
        error))
        return FALSE;

    if (payload->uid == 0 || payload->uidvalidity == 0) {
        g_set_error (error,
            G_IO_ERROR,
            code, "FlagChanged payload uid and uidvalidity are required");
        return FALSE;
    }

    switch (payload->mode) {
    case WYREBOX_FLAG_CHANGED_MODE_SET:
    case WYREBOX_FLAG_CHANGED_MODE_CLEAR:
    case WYREBOX_FLAG_CHANGED_MODE_REPLACE:
        break;
    default:
        g_set_error (error,
            G_IO_ERROR, code, "FlagChanged payload mode is invalid");
        return FALSE;
    }

    if (!validate_names ("system_flags", payload->system_flags,
        is_system_flag, code, error) ||
        !validate_names ("user_keywords", payload->user_keywords,
        is_user_keyword, code, error))
        return FALSE;

    if (payload->mode != WYREBOX_FLAG_CHANGED_MODE_REPLACE &&
        strv_count (payload->system_flags) == 0 &&
        strv_count (payload->user_keywords) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            code,
            "FlagChanged payload requires a system flag or user keyword");
        return FALSE;
    }

    return TRUE;
}

static gboolean
checked_add_string (gsize *total, const char *value, GError **error)
{
    gsize len = strlen (value);

    if (len > G_MAXUINT32 || len + sizeof (guint32) > G_MAXSIZE - *total) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "FlagChanged payload string is too large");
        return FALSE;
    }

    *total += sizeof (guint32) + len;
    return TRUE;
}

static void
write_string (guint8 **cursor, const char *value)
{
    gsize len = strlen (value);

    write_u32_le (*cursor, (guint32)len);
    *cursor += sizeof (guint32);
    memcpy (*cursor, value, len);
    *cursor += len;
}

static gboolean
read_string (const guint8 *data, gsize size, gsize *offset, char **out_value,
    GError **error)
{
    guint32 len = 0;

    *out_value = NULL;

    if (size - *offset < sizeof (guint32)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "FlagChanged payload is truncated");
        return FALSE;
    }

    len = read_u32_le (data + *offset);
    *offset += sizeof (guint32);
    if ((gsize)len > size - *offset) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "FlagChanged payload is truncated");
        return FALSE;
    }

    if (memchr (data + *offset, '\0', len) != NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "FlagChanged payload string contains embedded NUL");
        return FALSE;
    }

    *out_value = g_strndup ((const char *)data + *offset, len);
    *offset += len;
    return TRUE;
}

static gboolean
read_strv (const guint8 *data, gsize size, gsize *offset, guint32 count,
    GStrv *out_values, GError **error)
{
    g_autoptr (GPtrArray) values = NULL;

    *out_values = NULL;
    if (count == 0)
        return TRUE;

    /* Every encoded string takes at least its length prefix. */
    if ((gsize)count > (size - *offset) / sizeof (guint32)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "FlagChanged payload is truncated");
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

void
wyrebox_flag_changed_payload_clear (WyreboxFlagChangedPayload *payload)
{
    if (payload == NULL)
        return;

    g_clear_pointer (&payload->account_id, g_free);
    g_clear_pointer (&payload->mailbox_id, g_free);
    g_clear_pointer (&payload->system_flags, g_strfreev);
    g_clear_pointer (&payload->user_keywords, g_strfreev);
    payload->uidvalidity = 0;
    payload->uid = 0;
    payload->mode = WYREBOX_FLAG_CHANGED_MODE_SET;
}

GBytes *
wyrebox_flag_changed_payload_encode (const WyreboxFlagChangedPayload *payload,
    GError **error)
{
    g_autofree guint8 *data = NULL;
    guint8 *cursor = NULL;
    gsize payload_len = WYREBOX_FLAG_CHANGED_PAYLOAD_HEADER_SIZE;
    gsize n_flags = 0;
    gsize n_keywords = 0;

    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (!validate_payload (payload, G_IO_ERROR_INVALID_ARGUMENT, error))
        return NULL;

    n_flags = strv_count (payload->system_flags);
    n_keywords = strv_count (payload->user_keywords);
    if (n_keywords > G_MAXUINT32) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "FlagChanged payload has too many user keywords");
        return NULL;
    }

    if (!checked_add_string (&payload_len, payload->account_id, error) ||
        !checked_add_string (&payload_len, payload->mailbox_id, error))
        return NULL;
    for (gsize i = 0; i < n_flags; i++) {
        if (!checked_add_string (&payload_len, payload->system_flags[i],
            error))
            return NULL;
    }
    for (gsize i = 0; i < n_keywords; i++) {
        if (!checked_add_string (&payload_len, payload->user_keywords[i],
            error))
            return NULL;
    }

    data = g_malloc0 (payload_len);
    memcpy (data, WYREBOX_FLAG_CHANGED_PAYLOAD_MAGIC,
        WYREBOX_FLAG_CHANGED_PAYLOAD_MAGIC_LEN);
    write_u64_le (data + 8, payload->uid);
    write_u64_le (data + 16, payload->uidvalidity);
    data[24] = (guint8)payload->mode;
    write_u32_le (data + 25, (guint32)n_flags);
    write_u32_le (data + 29, (guint32)n_keywords);

    cursor = data + WYREBOX_FLAG_CHANGED_PAYLOAD_HEADER_SIZE;
    write_string (&cursor, payload->account_id);
    write_string (&cursor, payload->mailbox_id);
    for (gsize i = 0; i < n_flags; i++)
        write_string (&cursor, payload->system_flags[i]);
    for (gsize i = 0; i < n_keywords; i++)
        write_string (&cursor, payload->user_keywords[i]);
    g_assert (cursor == data + payload_len);

    return g_bytes_new_take (g_steal_pointer (&data), payload_len);
}

gboolean
wyrebox_flag_changed_payload_decode (GBytes *bytes,
    WyreboxFlagChangedPayload *out_payload, GError **error)
{
    const guint8 *data = NULL;
    gsize size = 0;
    gsize offset = WYREBOX_FLAG_CHANGED_PAYLOAD_HEADER_SIZE;
    g_auto (WyreboxFlagChangedPayload) decoded = { 0 };

    g_return_val_if_fail (bytes != NULL, FALSE);
    g_return_val_if_fail (out_payload != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    data = g_bytes_get_data (bytes, &size);
    if (size < WYREBOX_FLAG_CHANGED_PAYLOAD_HEADER_SIZE) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "FlagChanged payload is truncated");
        return FALSE;
    }

    if (memcmp (data, WYREBOX_FLAG_CHANGED_PAYLOAD_MAGIC,
        WYREBOX_FLAG_CHANGED_PAYLOAD_MAGIC_LEN) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "invalid FlagChanged payload magic");
        return FALSE;
    }

    decoded.uid = read_u64_le (data + 8);
    decoded.uidvalidity = read_u64_le (data + 16);
    decoded.mode = (WyreboxFlagChangedMode)data[24];

    if (!read_string (data, size, &offset, &decoded.account_id, error) ||
        !read_string (data, size, &offset, &decoded.mailbox_id, error) ||
        !read_strv (data, size, &offset, read_u32_le (data + 25),
        &decoded.system_flags, error) ||
        !read_strv (data, size, &offset, read_u32_le (data + 29),
        &decoded.user_keywords, error))
        return FALSE;

    if (offset != size) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "FlagChanged payload length is malformed");
        return FALSE;
    }

    if (!validate_payload (&decoded, G_IO_ERROR_INVALID_DATA, error))
        return FALSE;

    *out_payload = decoded;
    memset (&decoded, 0, sizeof (decoded));
    return TRUE;
}
