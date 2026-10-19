#include "wyrebox-message-appended-payload.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

static const char *valid_object_key =
    "sha256:"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static char *sample_flags[] = { (char *)"\\Seen", (char *)"\\Flagged", NULL };
static char *sample_keywords[] = { (char *)"project-x", (char *)"$Junk", NULL };

static WyreboxMessageAppendedPayload
minimal_payload (void)
{
    return (WyreboxMessageAppendedPayload) {
               .account_id = (char *)"account-1",
               .mailbox_id = (char *)"archive:account-1",
               .uidvalidity = 4,
               .uid = 11,
               .object_key = (char *)valid_object_key,
               .size_bytes = 12345,
    };
}

static WyreboxMessageAppendedPayload
full_payload (void)
{
    WyreboxMessageAppendedPayload payload = minimal_payload ();

    payload.internal_date_unix_us = 1700000000000000ULL;
    payload.message_id = (char *)"<message@example.test>";
    payload.subject = (char *)"Quarterly report";
    payload.from = (char *)"Alice <alice@example.test>";
    payload.to = (char *)"Bob <bob@example.test>";
    payload.date = (char *)"Tue, 14 Nov 2023 22:13:20 +0000";
    payload.duplicate_message_id_count = 2;
    payload.message_id_span_valid = TRUE;
    payload.message_id_span_start = 10;
    payload.message_id_span_end = 40;
    payload.subject_span_valid = TRUE;
    payload.subject_span_start = 41;
    payload.subject_span_end = 70;
    payload.system_flags = sample_flags;
    payload.user_keywords = sample_keywords;
    return payload;
}

static void
assert_encode_fails (const WyreboxMessageAppendedPayload *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;

    encoded = wyrebox_message_appended_payload_encode (payload, &error);
    g_assert_null (encoded);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_decode_fails_invalid_data (GBytes *bytes)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxMessageAppendedPayload) decoded = { 0 };

    g_assert_false (wyrebox_message_appended_payload_decode (bytes, &decoded,
        &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_null (decoded.account_id);
    g_assert_null (decoded.object_key);
}

static void
assert_strv_equal (GStrv actual, GStrv expected)
{
    g_assert_cmpuint (actual != NULL ? g_strv_length (actual) : 0, ==,
        expected != NULL ? g_strv_length (expected) : 0);
    for (gsize i = 0; expected != NULL && expected[i] != NULL; i++)
        g_assert_cmpstr (actual[i], ==, expected[i]);
}

static void
assert_round_trip (const WyreboxMessageAppendedPayload *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) reencoded = NULL;
    g_auto (WyreboxMessageAppendedPayload) decoded = { 0 };

    encoded = wyrebox_message_appended_payload_encode (payload, &error);
    g_assert_no_error (error);
    g_assert_nonnull (encoded);
    g_assert_true (wyrebox_message_appended_payload_decode (encoded, &decoded,
        &error));
    g_assert_no_error (error);

    g_assert_cmpstr (decoded.account_id, ==, payload->account_id);
    g_assert_cmpstr (decoded.mailbox_id, ==, payload->mailbox_id);
    g_assert_cmpuint (decoded.uidvalidity, ==, payload->uidvalidity);
    g_assert_cmpuint (decoded.uid, ==, payload->uid);
    g_assert_cmpstr (decoded.object_key, ==, payload->object_key);
    g_assert_cmpuint (decoded.size_bytes, ==, payload->size_bytes);
    g_assert_cmpuint (decoded.internal_date_unix_us, ==,
        payload->internal_date_unix_us);
    g_assert_cmpstr (decoded.message_id, ==, payload->message_id);
    g_assert_cmpstr (decoded.subject, ==, payload->subject);
    g_assert_cmpstr (decoded.from, ==, payload->from);
    g_assert_cmpstr (decoded.to, ==, payload->to);
    g_assert_cmpstr (decoded.cc, ==, payload->cc);
    g_assert_cmpstr (decoded.bcc, ==, payload->bcc);
    g_assert_cmpstr (decoded.date, ==, payload->date);
    g_assert_cmpuint (decoded.duplicate_message_id_count, ==,
        payload->duplicate_message_id_count);
    g_assert_cmpint (decoded.message_id_span_valid, ==,
        payload->message_id_span_valid);
    g_assert_cmpuint (decoded.message_id_span_start, ==,
        payload->message_id_span_start);
    g_assert_cmpuint (decoded.message_id_span_end, ==,
        payload->message_id_span_end);
    g_assert_cmpint (decoded.subject_span_valid, ==,
        payload->subject_span_valid);
    g_assert_cmpuint (decoded.subject_span_start, ==,
        payload->subject_span_start);
    g_assert_cmpuint (decoded.subject_span_end, ==,
        payload->subject_span_end);
    assert_strv_equal (decoded.system_flags, payload->system_flags);
    assert_strv_equal (decoded.user_keywords, payload->user_keywords);

    reencoded = wyrebox_message_appended_payload_encode (&decoded, &error);
    g_assert_no_error (error);
    g_assert_true (g_bytes_equal (encoded, reencoded));
}

static void
test_round_trip_full (void)
{
    WyreboxMessageAppendedPayload payload = full_payload ();
    g_autoptr (GBytes) encoded = NULL;

    assert_round_trip (&payload);

    encoded = wyrebox_message_appended_payload_encode (&payload, NULL);
    g_assert_cmpmem (g_bytes_get_data (encoded, NULL), 8, "WYREMAP1", 8);
}

static void
test_round_trip_minimal_keeps_null_headers (void)
{
    WyreboxMessageAppendedPayload payload = minimal_payload ();
    g_autoptr (GBytes) encoded = NULL;
    g_auto (WyreboxMessageAppendedPayload) decoded = { 0 };

    assert_round_trip (&payload);

    encoded = wyrebox_message_appended_payload_encode (&payload, NULL);
    g_assert_true (wyrebox_message_appended_payload_decode (encoded, &decoded,
        NULL));
    g_assert_null (decoded.message_id);
    g_assert_null (decoded.subject);
    g_assert_null (decoded.cc);
    g_assert_null (decoded.system_flags);
    g_assert_null (decoded.user_keywords);
    g_assert_false (decoded.message_id_span_valid);
    g_assert_false (decoded.subject_span_valid);
}

static void
test_round_trip_distinguishes_empty_from_null_headers (void)
{
    WyreboxMessageAppendedPayload payload = minimal_payload ();
    g_autoptr (GBytes) encoded = NULL;
    g_auto (WyreboxMessageAppendedPayload) decoded = { 0 };

    payload.subject = (char *)"";
    payload.cc = NULL;
    encoded = wyrebox_message_appended_payload_encode (&payload, NULL);
    g_assert_true (wyrebox_message_appended_payload_decode (encoded, &decoded,
        NULL));
    g_assert_cmpstr (decoded.subject, ==, "");
    g_assert_null (decoded.cc);
}

static void
test_round_trip_one_span_only (void)
{
    WyreboxMessageAppendedPayload payload = minimal_payload ();

    payload.subject_span_valid = TRUE;
    payload.subject_span_start = 5;
    payload.subject_span_end = 5;
    assert_round_trip (&payload);
}

static void
test_rejects_invalid_encode_inputs (void)
{
    WyreboxMessageAppendedPayload payload;
    char *dup_flags[] = { (char *)"\\Seen", (char *)"\\Seen", NULL };
    char *bad_flags[] = { (char *)"\\Recent", NULL };
    char *bad_keywords[] = { (char *)"\\Seen", NULL };
    char *space_keywords[] = { (char *)"two words", NULL };
    char *empty_keywords[] = { (char *)"", NULL };
    char *dup_keywords[] = { (char *)"a", (char *)"a", NULL };

    assert_encode_fails (NULL);

    payload = minimal_payload ();
    payload.account_id = NULL;
    assert_encode_fails (&payload);
    payload.account_id = (char *)"";
    assert_encode_fails (&payload);

    payload = minimal_payload ();
    payload.mailbox_id = NULL;
    assert_encode_fails (&payload);
    payload.mailbox_id = (char *)"";
    assert_encode_fails (&payload);

    payload = minimal_payload ();
    payload.uidvalidity = 0;
    assert_encode_fails (&payload);

    payload = minimal_payload ();
    payload.uid = 0;
    assert_encode_fails (&payload);

    payload = minimal_payload ();
    payload.object_key = NULL;
    assert_encode_fails (&payload);
    payload.object_key = (char *)"sha256:abc";
    assert_encode_fails (&payload);
    payload.object_key = (char *)
        "sha256:0123456789ABCDEF0123456789abcdef0123456789abcdef"
        "0123456789abcdef";
    assert_encode_fails (&payload);
    payload.object_key = (char *)
        "md5:0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0";
    assert_encode_fails (&payload);

    payload = minimal_payload ();
    payload.system_flags = dup_flags;
    assert_encode_fails (&payload);
    payload.system_flags = bad_flags;
    assert_encode_fails (&payload);

    payload = minimal_payload ();
    payload.user_keywords = bad_keywords;
    assert_encode_fails (&payload);
    payload.user_keywords = space_keywords;
    assert_encode_fails (&payload);
    payload.user_keywords = empty_keywords;
    assert_encode_fails (&payload);
    payload.user_keywords = dup_keywords;
    assert_encode_fails (&payload);

    payload = minimal_payload ();
    payload.message_id_span_valid = TRUE;
    payload.message_id_span_start = 9;
    payload.message_id_span_end = 3;
    assert_encode_fails (&payload);

    payload = minimal_payload ();
    payload.subject_span_start = 1;
    assert_encode_fails (&payload);
}

static GBytes *
encode_full (void)
{
    WyreboxMessageAppendedPayload payload = full_payload ();

    return wyrebox_message_appended_payload_encode (&payload, NULL);
}

static void
test_rejects_malformed_decode_inputs (void)
{
    g_autoptr (GBytes) encoded = encode_full ();
    g_autoptr (GBytes) empty = g_bytes_new_static ("", 0);
    g_autoptr (GBytes) short_magic = g_bytes_new_static ("WYREMAP", 7);
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);

    assert_decode_fails_invalid_data (empty);
    assert_decode_fails_invalid_data (short_magic);

    /* Every strict prefix is truncated. */
    for (gsize len = 0; len < size; len++) {
        g_autoptr (GBytes) prefix = g_bytes_new (data, len);

        assert_decode_fails_invalid_data (prefix);
    }

    {
        g_autofree guint8 *copy = g_memdup2 (data, size);
        g_autoptr (GBytes) bad = NULL;

        copy[7] = '2';
        bad = g_bytes_new (copy, size);
        assert_decode_fails_invalid_data (bad);
    }

    {
        g_autofree guint8 *copy = g_malloc (size + 1);
        g_autoptr (GBytes) bad = NULL;

        memcpy (copy, data, size);
        copy[size] = 0;
        bad = g_bytes_new (copy, size + 1);
        assert_decode_fails_invalid_data (bad);
    }
}

static void
test_rejects_invalid_decoded_fields (void)
{
    g_autoptr (GBytes) encoded = encode_full ();
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);

    /* uidvalidity (8), uid (16), span flags (44), span order (48). */
    const gsize zero_offsets[] = { 8, 16 };

    for (gsize i = 0; i < G_N_ELEMENTS (zero_offsets); i++) {
        g_autofree guint8 *copy = g_memdup2 (data, size);
        g_autoptr (GBytes) bad = NULL;

        memset (copy + zero_offsets[i], 0, 8);
        bad = g_bytes_new (copy, size);
        assert_decode_fails_invalid_data (bad);
    }

    {
        g_autofree guint8 *copy = g_memdup2 (data, size);
        g_autoptr (GBytes) bad = NULL;

        copy[44] = 0x4;
        bad = g_bytes_new (copy, size);
        assert_decode_fails_invalid_data (bad);
    }

    {
        g_autofree guint8 *copy = g_memdup2 (data, size);
        g_autoptr (GBytes) bad = NULL;

        /* message id span start beyond end */
        memset (copy + 48, 0xff, 8);
        bad = g_bytes_new (copy, size);
        assert_decode_fails_invalid_data (bad);
    }

    {
        g_autofree guint8 *copy = g_memdup2 (data, size);
        g_autoptr (GBytes) bad = NULL;

        /* span flags cleared while span values are still set */
        memset (copy + 44, 0, 4);
        bad = g_bytes_new (copy, size);
        assert_decode_fails_invalid_data (bad);
    }
}

static void
test_rejects_embedded_nul_and_null_flag (void)
{
    WyreboxMessageAppendedPayload payload = minimal_payload ();
    g_autoptr (GBytes) encoded = NULL;
    gsize size = 0;
    const guint8 *data = NULL;

    payload.account_id = (char *)"ab";
    encoded = wyrebox_message_appended_payload_encode (&payload, NULL);
    data = g_bytes_get_data (encoded, &size);

    {
        g_autofree guint8 *copy = g_memdup2 (data, size);
        g_autoptr (GBytes) bad = NULL;

        /* account_id string bytes start after the u32 length prefix. */
        copy[80 + 4] = '\0';
        bad = g_bytes_new (copy, size);
        assert_decode_fails_invalid_data (bad);
    }
    g_clear_pointer (&encoded, g_bytes_unref);

    payload = minimal_payload ();
    payload.system_flags = sample_flags;
    encoded = wyrebox_message_appended_payload_encode (&payload, NULL);
    data = g_bytes_get_data (encoded, &size);

    {
        g_autofree guint8 *copy = g_memdup2 (data, size);
        g_autoptr (GBytes) bad = NULL;
        /*
         * The tail is: flag count, "\Seen", "\Flagged", keyword count, each
         * string with a u32 length prefix. Locate the first flag's prefix
         * and turn it into the NULL sentinel.
         */
        gsize first_flag_prefix = size - sizeof (guint32) -
            (sizeof (guint32) + strlen ("\\Flagged")) -
            (sizeof (guint32) + strlen ("\\Seen"));

        memset (copy + first_flag_prefix, 0xff, 4);
        bad = g_bytes_new (copy, size);
        assert_decode_fails_invalid_data (bad);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/journal/message-appended-payload/round-trip-full",
        test_round_trip_full);
    g_test_add_func ("/journal/message-appended-payload/round-trip-minimal",
        test_round_trip_minimal_keeps_null_headers);
    g_test_add_func ("/journal/message-appended-payload/empty-vs-null",
        test_round_trip_distinguishes_empty_from_null_headers);
    g_test_add_func ("/journal/message-appended-payload/one-span-only",
        test_round_trip_one_span_only);
    g_test_add_func ("/journal/message-appended-payload/invalid-encode",
        test_rejects_invalid_encode_inputs);
    g_test_add_func ("/journal/message-appended-payload/malformed-decode",
        test_rejects_malformed_decode_inputs);
    g_test_add_func ("/journal/message-appended-payload/invalid-fields",
        test_rejects_invalid_decoded_fields);
    g_test_add_func ("/journal/message-appended-payload/nul-and-null-flag",
        test_rejects_embedded_nul_and_null_flag);

    return g_test_run ();
}
