#include "wyrebox-memberships-expunged-payload.h"

#include <gio/gio.h>
#include <string.h>

#define WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_MAGIC "WYREMEX1"
#define WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_MAGIC_LEN 8
#define WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_HEADER_SIZE 20

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
validate_required_string (const char *field_name, const char *value,
    GIOErrorEnum code, GError **error)
{
    if (value == NULL || value[0] == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsExpunged payload %s is required", field_name);
        return FALSE;
    }

    return TRUE;
}

static gboolean
validate_payload (const WyreboxMembershipsExpungedPayload *payload,
    GIOErrorEnum code, GError **error)
{
    if (payload == NULL) {
        g_set_error (error,
            G_IO_ERROR, code, "MembershipsExpunged payload is required");
        return FALSE;
    }

    if (!validate_required_string ("account_id", payload->account_id, code,
        error) ||
        !validate_required_string ("mailbox_id", payload->mailbox_id, code,
        error))
        return FALSE;

    if (payload->uidvalidity == 0) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsExpunged payload uidvalidity is required");
        return FALSE;
    }

    if (payload->n_uids == 0 || payload->uids == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsExpunged payload requires at least one uid");
        return FALSE;
    }

    if (payload->n_uids > G_MAXUINT32) {
        g_set_error (error,
            G_IO_ERROR,
            code, "MembershipsExpunged payload has too many uids");
        return FALSE;
    }

    for (gsize i = 0; i < payload->n_uids; i++) {
        if (payload->uids[i] == 0) {
            g_set_error (error,
                G_IO_ERROR,
                code, "MembershipsExpunged payload uid must be at least 1");
            return FALSE;
        }

        if (i > 0 && payload->uids[i] <= payload->uids[i - 1]) {
            g_set_error (error,
                G_IO_ERROR,
                code,
                "MembershipsExpunged payload uids must be strictly ascending");
            return FALSE;
        }
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
            "MembershipsExpunged payload string is too large");
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
            G_IO_ERROR_INVALID_DATA,
            "MembershipsExpunged payload is truncated");
        return FALSE;
    }

    len = read_u32_le (data + *offset);
    *offset += sizeof (guint32);
    if ((gsize)len > size - *offset) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MembershipsExpunged payload is truncated");
        return FALSE;
    }

    if (memchr (data + *offset, '\0', len) != NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MembershipsExpunged payload string contains embedded NUL");
        return FALSE;
    }

    *out_value = g_strndup ((const char *)data + *offset, len);
    *offset += len;
    return TRUE;
}

void
wyrebox_memberships_expunged_payload_clear (
    WyreboxMembershipsExpungedPayload *payload)
{
    if (payload == NULL)
        return;

    g_clear_pointer (&payload->account_id, g_free);
    g_clear_pointer (&payload->mailbox_id, g_free);
    g_clear_pointer (&payload->uids, g_free);
    payload->uidvalidity = 0;
    payload->n_uids = 0;
}

GBytes *
wyrebox_memberships_expunged_payload_encode (
    const WyreboxMembershipsExpungedPayload *payload, GError **error)
{
    g_autofree guint8 *data = NULL;
    guint8 *cursor = NULL;
    gsize payload_len = WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_HEADER_SIZE;
    gsize uids_bytes = 0;

    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (!validate_payload (payload, G_IO_ERROR_INVALID_ARGUMENT, error))
        return NULL;

    if (!checked_add_string (&payload_len, payload->account_id, error) ||
        !checked_add_string (&payload_len, payload->mailbox_id, error))
        return NULL;

    if (payload->n_uids > (G_MAXSIZE - payload_len) / sizeof (guint64)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "MembershipsExpunged payload is too large");
        return NULL;
    }

    uids_bytes = payload->n_uids * sizeof (guint64);
    payload_len += uids_bytes;

    data = g_malloc0 (payload_len);
    memcpy (data, WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_MAGIC,
        WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_MAGIC_LEN);
    write_u64_le (data + 8, payload->uidvalidity);
    write_u32_le (data + 16, (guint32)payload->n_uids);

    cursor = data + WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_HEADER_SIZE;
    write_string (&cursor, payload->account_id);
    write_string (&cursor, payload->mailbox_id);
    for (gsize i = 0; i < payload->n_uids; i++) {
        write_u64_le (cursor, payload->uids[i]);
        cursor += sizeof (guint64);
    }
    g_assert (cursor == data + payload_len);

    return g_bytes_new_take (g_steal_pointer (&data), payload_len);
}

gboolean
wyrebox_memberships_expunged_payload_decode (GBytes *bytes,
    WyreboxMembershipsExpungedPayload *out_payload, GError **error)
{
    const guint8 *data = NULL;
    gsize size = 0;
    gsize offset = WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_HEADER_SIZE;
    guint32 n_uids = 0;
    g_auto (WyreboxMembershipsExpungedPayload) decoded = { 0 };

    g_return_val_if_fail (bytes != NULL, FALSE);
    g_return_val_if_fail (out_payload != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    data = g_bytes_get_data (bytes, &size);
    if (size < WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_HEADER_SIZE) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MembershipsExpunged payload is truncated");
        return FALSE;
    }

    if (memcmp (data, WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_MAGIC,
        WYREBOX_MEMBERSHIPS_EXPUNGED_PAYLOAD_MAGIC_LEN) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "invalid MembershipsExpunged payload magic");
        return FALSE;
    }

    decoded.uidvalidity = read_u64_le (data + 8);
    n_uids = read_u32_le (data + 16);

    if (!read_string (data, size, &offset, &decoded.account_id, error) ||
        !read_string (data, size, &offset, &decoded.mailbox_id, error))
        return FALSE;

    if (n_uids == 0 ||
        (gsize)n_uids > (size - offset) / sizeof (guint64)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MembershipsExpunged payload is truncated");
        return FALSE;
    }

    decoded.uids = g_new (guint64, n_uids);
    decoded.n_uids = n_uids;
    for (guint32 i = 0; i < n_uids; i++) {
        decoded.uids[i] = read_u64_le (data + offset);
        offset += sizeof (guint64);
    }

    if (offset != size) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "MembershipsExpunged payload length is malformed");
        return FALSE;
    }

    if (!validate_payload (&decoded, G_IO_ERROR_INVALID_DATA, error))
        return FALSE;

    *out_payload = decoded;
    memset (&decoded, 0, sizeof (decoded));
    return TRUE;
}
