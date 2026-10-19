#include "wyrebox-message-appended-payload.h"

#include <gio/gio.h>
#include <string.h>

#define MESSAGE_APPENDED_PAYLOAD_MAGIC "WYREMAP1"
#define MESSAGE_APPENDED_PAYLOAD_MAGIC_LEN 8
#define MESSAGE_APPENDED_PAYLOAD_HEADER_SIZE 80
#define MESSAGE_APPENDED_NULL_STRING G_MAXUINT32
#define MESSAGE_APPENDED_SPAN_FLAG_MESSAGE_ID 0x1u
#define MESSAGE_APPENDED_SPAN_FLAG_SUBJECT 0x2u
#define MESSAGE_APPENDED_SPAN_FLAGS_MASK 0x3u
#define SHA256_OBJECT_KEY_PREFIX "sha256:"
#define SHA256_OBJECT_KEY_PREFIX_LEN 7
#define SHA256_OBJECT_KEY_LEN 71

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
is_sha256_object_key (const char *value)
{
    if (value == NULL || strlen (value) != SHA256_OBJECT_KEY_LEN ||
        !g_str_has_prefix (value, SHA256_OBJECT_KEY_PREFIX))
        return FALSE;

    for (gsize i = SHA256_OBJECT_KEY_PREFIX_LEN; i < SHA256_OBJECT_KEY_LEN;
        i++) {
        if (!g_ascii_isxdigit (value[i]) || g_ascii_isupper (value[i]))
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
            code, "MessageAppended payload %s is required", field_name);
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
                code, "MessageAppended payload %s contains an invalid value",
                field_name);
            return FALSE;
        }

        for (gsize j = 0; j < i; j++) {
            if (strcmp (values[i], values[j]) == 0) {
                g_set_error (error,
                    G_IO_ERROR,
                    code,
                    "MessageAppended payload %s contains duplicate values",
                    field_name);
                return FALSE;
            }
        }
    }

    return TRUE;
}

static gboolean
validate_span (const char *field_name, gboolean valid, guint64 start,
    guint64 end, GIOErrorEnum code, GError **error)
{
    if ((valid && start > end) || (!valid && (start != 0 || end != 0))) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MessageAppended payload %s span is malformed", field_name);
        return FALSE;
    }

    return TRUE;
}

static gboolean
validate_payload (const WyreboxMessageAppendedPayload *payload,
    GIOErrorEnum code, GError **error)
{
    if (payload == NULL) {
        g_set_error (error,
            G_IO_ERROR, code, "MessageAppended payload is required");
        return FALSE;
    }

    if (!validate_required_string ("account_id", payload->account_id, code,
        error) ||
        !validate_required_string ("mailbox_id", payload->mailbox_id, code,
        error))
        return FALSE;

    if (payload->uidvalidity == 0 || payload->uid == 0) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MessageAppended payload uidvalidity and uid are required");
        return FALSE;
    }

    if (!is_sha256_object_key (payload->object_key)) {
        g_set_error (error,
            G_IO_ERROR,
            code,
            "MessageAppended payload object_key must be sha256:64 lowercase "
            "hex characters");
        return FALSE;
    }

    return validate_span ("message_id", payload->message_id_span_valid,
               payload->message_id_span_start, payload->message_id_span_end,
               code, error) &&
           validate_span ("subject", payload->subject_span_valid,
               payload->subject_span_start, payload->subject_span_end, code,
               error) &&
           validate_names ("system_flags", payload->system_flags,
               is_system_flag, code, error) &&
           validate_names ("user_keywords", payload->user_keywords,
               is_user_keyword, code, error);
}

static gboolean
append_nullable_string (GByteArray *buffer, const char *value, GError **error)
{
    guint8 prefix[sizeof (guint32)];
    gsize len = 0;

    if (value == NULL) {
        write_u32_le (prefix, MESSAGE_APPENDED_NULL_STRING);
        g_byte_array_append (buffer, prefix, sizeof (prefix));
        return TRUE;
    }

    len = strlen (value);
    if (len >= MESSAGE_APPENDED_NULL_STRING) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "MessageAppended payload string is too large");
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
    gsize count = values != NULL ? g_strv_length (values) : 0;

    if (count >= MESSAGE_APPENDED_NULL_STRING) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "MessageAppended payload has too many flags or keywords");
        return FALSE;
    }

    append_u32 (buffer, (guint32)count);
    for (gsize i = 0; i < count; i++) {
        if (!append_nullable_string (buffer, values[i], error))
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
            G_IO_ERROR_INVALID_DATA, "MessageAppended payload is truncated");
        return FALSE;
    }

    *out_value = read_u32_le (data + *offset);
    *offset += sizeof (guint32);
    return TRUE;
}

static gboolean
read_nullable_string (const guint8 *data, gsize size, gsize *offset,
    char **out_value, GError **error)
{
    guint32 len = 0;

    *out_value = NULL;

    if (!read_u32 (data, size, offset, &len, error))
        return FALSE;

    if (len == MESSAGE_APPENDED_NULL_STRING)
        return TRUE;

    if ((gsize)len > size - *offset) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "MessageAppended payload is truncated");
        return FALSE;
    }

    if (memchr (data + *offset, '\0', len) != NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MessageAppended payload string contains embedded NUL");
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
            G_IO_ERROR_INVALID_DATA, "MessageAppended payload is truncated");
        return FALSE;
    }

    values = g_ptr_array_new_with_free_func (g_free);
    for (guint32 i = 0; i < count; i++) {
        char *value = NULL;

        if (!read_nullable_string (data, size, offset, &value, error))
            return FALSE;

        if (value == NULL) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "MessageAppended payload flag or keyword is null");
            return FALSE;
        }

        g_ptr_array_add (values, value);
    }

    g_ptr_array_add (values, NULL);
    *out_values = (GStrv)g_ptr_array_free (g_steal_pointer (&values), FALSE);
    return TRUE;
}

void
wyrebox_message_appended_payload_clear (WyreboxMessageAppendedPayload *payload)
{
    if (payload == NULL)
        return;

    g_clear_pointer (&payload->account_id, g_free);
    g_clear_pointer (&payload->mailbox_id, g_free);
    g_clear_pointer (&payload->object_key, g_free);
    g_clear_pointer (&payload->message_id, g_free);
    g_clear_pointer (&payload->subject, g_free);
    g_clear_pointer (&payload->from, g_free);
    g_clear_pointer (&payload->to, g_free);
    g_clear_pointer (&payload->cc, g_free);
    g_clear_pointer (&payload->bcc, g_free);
    g_clear_pointer (&payload->date, g_free);
    g_clear_pointer (&payload->system_flags, g_strfreev);
    g_clear_pointer (&payload->user_keywords, g_strfreev);
    payload->uidvalidity = 0;
    payload->uid = 0;
    payload->size_bytes = 0;
    payload->internal_date_unix_us = 0;
    payload->duplicate_message_id_count = 0;
    payload->message_id_span_valid = FALSE;
    payload->message_id_span_start = 0;
    payload->message_id_span_end = 0;
    payload->subject_span_valid = FALSE;
    payload->subject_span_start = 0;
    payload->subject_span_end = 0;
}

GBytes *
wyrebox_message_appended_payload_encode (const WyreboxMessageAppendedPayload
    *payload, GError **error)
{
    g_autoptr (GByteArray) buffer = NULL;
    guint32 span_flags = 0;

    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (!validate_payload (payload, G_IO_ERROR_INVALID_ARGUMENT, error))
        return NULL;

    if (payload->message_id_span_valid)
        span_flags |= MESSAGE_APPENDED_SPAN_FLAG_MESSAGE_ID;
    if (payload->subject_span_valid)
        span_flags |= MESSAGE_APPENDED_SPAN_FLAG_SUBJECT;

    buffer = g_byte_array_new ();
    g_byte_array_append (buffer,
        (const guint8 *)MESSAGE_APPENDED_PAYLOAD_MAGIC,
        MESSAGE_APPENDED_PAYLOAD_MAGIC_LEN);
    append_u64 (buffer, payload->uidvalidity);
    append_u64 (buffer, payload->uid);
    append_u64 (buffer, payload->size_bytes);
    append_u64 (buffer, payload->internal_date_unix_us);
    append_u32 (buffer, payload->duplicate_message_id_count);
    append_u32 (buffer, span_flags);
    append_u64 (buffer, payload->message_id_span_start);
    append_u64 (buffer, payload->message_id_span_end);
    append_u64 (buffer, payload->subject_span_start);
    append_u64 (buffer, payload->subject_span_end);
    g_assert (buffer->len == MESSAGE_APPENDED_PAYLOAD_HEADER_SIZE);

    if (!append_nullable_string (buffer, payload->account_id, error) ||
        !append_nullable_string (buffer, payload->mailbox_id, error) ||
        !append_nullable_string (buffer, payload->object_key, error) ||
        !append_nullable_string (buffer, payload->message_id, error) ||
        !append_nullable_string (buffer, payload->subject, error) ||
        !append_nullable_string (buffer, payload->from, error) ||
        !append_nullable_string (buffer, payload->to, error) ||
        !append_nullable_string (buffer, payload->cc, error) ||
        !append_nullable_string (buffer, payload->bcc, error) ||
        !append_nullable_string (buffer, payload->date, error) ||
        !append_strv (buffer, payload->system_flags, error) ||
        !append_strv (buffer, payload->user_keywords, error))
        return NULL;

    return g_byte_array_free_to_bytes (g_steal_pointer (&buffer));
}

gboolean
wyrebox_message_appended_payload_decode (GBytes *bytes,
    WyreboxMessageAppendedPayload *out_payload, GError **error)
{
    g_auto (WyreboxMessageAppendedPayload) decoded = { 0 };
    const guint8 *data = NULL;
    gsize size = 0;
    gsize offset = MESSAGE_APPENDED_PAYLOAD_HEADER_SIZE;
    guint32 span_flags = 0;

    g_return_val_if_fail (bytes != NULL, FALSE);
    g_return_val_if_fail (out_payload != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    data = g_bytes_get_data (bytes, &size);
    if (size < MESSAGE_APPENDED_PAYLOAD_HEADER_SIZE) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "MessageAppended payload is truncated");
        return FALSE;
    }

    if (memcmp (data, MESSAGE_APPENDED_PAYLOAD_MAGIC,
        MESSAGE_APPENDED_PAYLOAD_MAGIC_LEN) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "invalid MessageAppended payload magic");
        return FALSE;
    }

    decoded.uidvalidity = read_u64_le (data + 8);
    decoded.uid = read_u64_le (data + 16);
    decoded.size_bytes = read_u64_le (data + 24);
    decoded.internal_date_unix_us = read_u64_le (data + 32);
    decoded.duplicate_message_id_count = read_u32_le (data + 40);
    span_flags = read_u32_le (data + 44);
    if ((span_flags & ~MESSAGE_APPENDED_SPAN_FLAGS_MASK) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MessageAppended payload has unknown span flags");
        return FALSE;
    }

    decoded.message_id_span_valid =
        (span_flags & MESSAGE_APPENDED_SPAN_FLAG_MESSAGE_ID) != 0;
    decoded.subject_span_valid =
        (span_flags & MESSAGE_APPENDED_SPAN_FLAG_SUBJECT) != 0;
    decoded.message_id_span_start = read_u64_le (data + 48);
    decoded.message_id_span_end = read_u64_le (data + 56);
    decoded.subject_span_start = read_u64_le (data + 64);
    decoded.subject_span_end = read_u64_le (data + 72);

    if (!read_nullable_string (data, size, &offset, &decoded.account_id,
        error) ||
        !read_nullable_string (data, size, &offset, &decoded.mailbox_id,
        error) ||
        !read_nullable_string (data, size, &offset, &decoded.object_key,
        error) ||
        !read_nullable_string (data, size, &offset, &decoded.message_id,
        error) ||
        !read_nullable_string (data, size, &offset, &decoded.subject, error) ||
        !read_nullable_string (data, size, &offset, &decoded.from, error) ||
        !read_nullable_string (data, size, &offset, &decoded.to, error) ||
        !read_nullable_string (data, size, &offset, &decoded.cc, error) ||
        !read_nullable_string (data, size, &offset, &decoded.bcc, error) ||
        !read_nullable_string (data, size, &offset, &decoded.date, error) ||
        !read_strv (data, size, &offset, &decoded.system_flags, error) ||
        !read_strv (data, size, &offset, &decoded.user_keywords, error))
        return FALSE;

    if (offset != size) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MessageAppended payload length is malformed");
        return FALSE;
    }

    if (!validate_payload (&decoded, G_IO_ERROR_INVALID_DATA, error))
        return FALSE;

    *out_payload = decoded;
    memset (&decoded, 0, sizeof (decoded));
    return TRUE;
}
