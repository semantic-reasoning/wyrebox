#include "wyrebox-facts-extracted-payload.h"

#include <gio/gio.h>
#include <string.h>

#define FACTS_EXTRACTED_PAYLOAD_MAGIC "WYREFEX1"
#define FACTS_EXTRACTED_PAYLOAD_MAGIC_LEN 8
#define FACTS_EXTRACTED_PAYLOAD_HEADER_SIZE 20

static void
write_u32_le (GByteArray *buffer, guint32 value)
{
    guint8 bytes[4] = {
        (guint8)(value & 0xff),
        (guint8)((value >> 8) & 0xff),
        (guint8)((value >> 16) & 0xff),
        (guint8)((value >> 24) & 0xff),
    };

    g_byte_array_append (buffer, bytes, sizeof (bytes));
}

static void
write_u64_le (GByteArray *buffer, guint64 value)
{
    write_u32_le (buffer, (guint32)(value & 0xffffffff));
    write_u32_le (buffer, (guint32)(value >> 32));
}

static guint32
read_u32_le (const guint8 *data)
{
    return (guint32)data[0] | ((guint32)data[1] << 8) |
           ((guint32)data[2] << 16) | ((guint32)data[3] << 24);
}

static guint64
read_u64_le (const guint8 *data)
{
    return (guint64)read_u32_le (data) |
           ((guint64)read_u32_le (data + 4) << 32);
}

static gboolean
write_string (GByteArray *buffer, const char *value, GError **error)
{
    gsize len = strlen (value);

    if (len == 0 || len > G_MAXUINT32) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "FactsExtracted payload strings must be non-empty and fit 32 bits");
        return FALSE;
    }

    write_u32_le (buffer, (guint32)len);
    g_byte_array_append (buffer, (const guint8 *)value, (guint)len);
    return TRUE;
}

static gboolean
read_u32 (const guint8 *data, gsize size, gsize *offset, guint32 *out_value,
    GError **error)
{
    if (size - *offset < sizeof (guint32)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "FactsExtracted payload is truncated");
        return FALSE;
    }

    *out_value = read_u32_le (data + *offset);
    *offset += sizeof (guint32);
    return TRUE;
}

static gboolean
read_string (const guint8 *data, gsize size, gsize *offset, char **out_value,
    GError **error)
{
    guint32 len = 0;

    if (!read_u32 (data, size, offset, &len, error))
        return FALSE;

    if ((gsize)len > size - *offset) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA, "FactsExtracted payload is truncated");
        return FALSE;
    }

    if (len == 0 || memchr (data + *offset, '\0', len) != NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "FactsExtracted payload string is empty or contains NUL");
        return FALSE;
    }

    *out_value = g_strndup ((const char *)data + *offset, len);
    *offset += len;
    return TRUE;
}

static gboolean
validate_fact (const WyreboxFactRecord *fact, const char *message_id,
    GIOErrorEnum code, GError **error)
{
    if (fact == NULL || fact->predicate == NULL || fact->source == NULL ||
        fact->args == NULL || g_strcmp0 (fact->args[0], message_id) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            code,
            "FactsExtracted payload fact must name message %s as its first "
            "argument", message_id);
        return FALSE;
    }

    return TRUE;
}

void
wyrebox_facts_extracted_payload_fact_free (gpointer fact)
{
    if (fact == NULL)
        return;

    wyrebox_fact_record_clear (fact);
    g_free (fact);
}

void
wyrebox_facts_extracted_payload_clear (WyreboxFactsExtractedPayload *payload)
{
    if (payload == NULL)
        return;

    g_clear_pointer (&payload->account_id, g_free);
    g_clear_pointer (&payload->message_id, g_free);
    g_clear_pointer (&payload->facts, g_ptr_array_unref);
    payload->extracted_at_unix_us = 0;
}

GBytes *
wyrebox_facts_extracted_payload_encode (const WyreboxFactsExtractedPayload
    *payload, GError **error)
{
    g_autoptr (GByteArray) buffer = NULL;
    guint n_facts = 0;

    g_return_val_if_fail (payload != NULL, NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (payload->account_id == NULL || payload->account_id[0] == '\0' ||
        payload->message_id == NULL || payload->message_id[0] == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "FactsExtracted payload needs an account and a message");
        return NULL;
    }

    n_facts = payload->facts != NULL ? payload->facts->len : 0;
    buffer = g_byte_array_new ();
    g_byte_array_append (buffer, (const guint8 *)FACTS_EXTRACTED_PAYLOAD_MAGIC,
        FACTS_EXTRACTED_PAYLOAD_MAGIC_LEN);
    write_u64_le (buffer, payload->extracted_at_unix_us);
    write_u32_le (buffer, n_facts);
    if (!write_string (buffer, payload->account_id, error) ||
        !write_string (buffer, payload->message_id, error))
        return NULL;

    for (guint i = 0; i < n_facts; i++) {
        const WyreboxFactRecord *fact = g_ptr_array_index (payload->facts, i);

        if (!validate_fact (fact, payload->message_id,
            G_IO_ERROR_INVALID_ARGUMENT, error) ||
            !write_string (buffer, fact->predicate, error) ||
            !write_string (buffer, fact->source, error))
            return NULL;

        write_u32_le (buffer, fact->confidence_ppm);
        write_u32_le (buffer, g_strv_length (fact->args));
        for (guint j = 0; fact->args[j] != NULL; j++) {
            if (!write_string (buffer, fact->args[j], error))
                return NULL;
        }
    }

    return g_byte_array_free_to_bytes (g_steal_pointer (&buffer));
}

static WyreboxFactRecord *
read_fact (const guint8 *data, gsize size, gsize *offset,
    guint64 created_at_unix_us, GError **error)
{
    g_autofree char *predicate = NULL;
    g_autofree char *source = NULL;
    g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
    g_autoptr (GError) local_error = NULL;
    WyreboxFactRecord *fact = NULL;
    guint32 confidence_ppm = 0;
    guint32 n_args = 0;

    if (!read_string (data, size, offset, &predicate, error) ||
        !read_string (data, size, offset, &source, error) ||
        !read_u32 (data, size, offset, &confidence_ppm, error) ||
        !read_u32 (data, size, offset, &n_args, error))
        return NULL;

    for (guint32 i = 0; i < n_args; i++) {
        char *arg = NULL;

        if (!read_string (data, size, offset, &arg, error))
            return NULL;
        g_ptr_array_add (args, arg);
    }
    g_ptr_array_add (args, NULL);

    fact = g_new0 (WyreboxFactRecord, 1);
    if (!wyrebox_fact_record_init (fact, predicate,
        (const char *const *)args->pdata, source, confidence_ppm,
        created_at_unix_us, &local_error)) {
        g_free (fact);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "FactsExtracted payload fact is malformed: %s",
            local_error->message);
        return NULL;
    }

    return fact;
}

gboolean
wyrebox_facts_extracted_payload_decode (GBytes *bytes,
    WyreboxFactsExtractedPayload *out_payload, GError **error)
{
    g_auto (WyreboxFactsExtractedPayload) decoded = { 0 };
    const guint8 *data = NULL;
    gsize size = 0;
    gsize offset = FACTS_EXTRACTED_PAYLOAD_HEADER_SIZE;
    guint32 n_facts = 0;

    g_return_val_if_fail (bytes != NULL, FALSE);
    g_return_val_if_fail (out_payload != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    data = g_bytes_get_data (bytes, &size);
    if (size < FACTS_EXTRACTED_PAYLOAD_HEADER_SIZE ||
        memcmp (data, FACTS_EXTRACTED_PAYLOAD_MAGIC,
        FACTS_EXTRACTED_PAYLOAD_MAGIC_LEN) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "FactsExtracted payload is truncated or has invalid magic");
        return FALSE;
    }

    decoded.extracted_at_unix_us = read_u64_le (data + 8);
    n_facts = read_u32_le (data + 16);
    decoded.facts = g_ptr_array_new_with_free_func
            (wyrebox_facts_extracted_payload_fact_free);
    if (!read_string (data, size, &offset, &decoded.account_id, error) ||
        !read_string (data, size, &offset, &decoded.message_id, error))
        return FALSE;

    for (guint32 i = 0; i < n_facts; i++) {
        WyreboxFactRecord *fact = read_fact (data, size, &offset,
                decoded.extracted_at_unix_us, error);

        if (fact == NULL)
            return FALSE;
        g_ptr_array_add (decoded.facts, fact);
        if (!validate_fact (fact, decoded.message_id, G_IO_ERROR_INVALID_DATA,
            error))
            return FALSE;
    }

    if (offset != size) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "FactsExtracted payload length is malformed");
        return FALSE;
    }

    wyrebox_facts_extracted_payload_clear (out_payload);
    *out_payload = decoded;
    memset (&decoded, 0, sizeof (decoded));
    return TRUE;
}
