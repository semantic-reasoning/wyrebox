#include "wyrebox-flag-changed-payload.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#define HEADER_SIZE 33

static char *seen_flagged[] = { (char *)"\\Seen", (char *)"\\Flagged", NULL };
static char *one_keyword[] = { (char *)"project-x", NULL };

static WyreboxFlagChangedPayload
valid_payload (void)
{
    return (WyreboxFlagChangedPayload) {
               .account_id = (char *)"account-1",
               .mailbox_id = (char *)"inbox:account-1",
               .uidvalidity = 1,
               .uid = 7,
               .mode = WYREBOX_FLAG_CHANGED_MODE_SET,
               .system_flags = seen_flagged,
               .user_keywords = one_keyword,
    };
}

static GBytes *
encode_valid_payload (void)
{
    WyreboxFlagChangedPayload payload = valid_payload ();
    g_autoptr (GError) error = NULL;
    GBytes *encoded = NULL;

    encoded = wyrebox_flag_changed_payload_encode (&payload, &error);
    g_assert_no_error (error);
    g_assert_nonnull (encoded);
    return encoded;
}

static void
assert_encode_fails (const WyreboxFlagChangedPayload *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;

    encoded = wyrebox_flag_changed_payload_encode (payload, &error);
    g_assert_null (encoded);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_decode_fails_invalid_data (GBytes *bytes)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxFlagChangedPayload) decoded = { 0 };

    g_assert_false (wyrebox_flag_changed_payload_decode (bytes, &decoded,
        &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_null (decoded.account_id);
}

static void
assert_strv_equal (char **actual, char **expected)
{
    if (expected == NULL || expected[0] == NULL) {
        g_assert_true (actual == NULL || actual[0] == NULL);
        return;
    }

    g_assert_nonnull (actual);
    g_assert_true (g_strv_equal ((const char *const *)actual,
        (const char *const *)expected));
}

static void
assert_round_trip (const WyreboxFlagChangedPayload *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) reencoded = NULL;
    g_auto (WyreboxFlagChangedPayload) decoded = { 0 };

    encoded = wyrebox_flag_changed_payload_encode (payload, &error);
    g_assert_no_error (error);
    g_assert_nonnull (encoded);
    g_assert_true (wyrebox_flag_changed_payload_decode (encoded, &decoded,
        &error));
    g_assert_no_error (error);
    g_assert_cmpstr (decoded.account_id, ==, payload->account_id);
    g_assert_cmpstr (decoded.mailbox_id, ==, payload->mailbox_id);
    g_assert_cmpuint (decoded.uidvalidity, ==, payload->uidvalidity);
    g_assert_cmpuint (decoded.uid, ==, payload->uid);
    g_assert_cmpint (decoded.mode, ==, payload->mode);
    assert_strv_equal (decoded.system_flags, payload->system_flags);
    assert_strv_equal (decoded.user_keywords, payload->user_keywords);

    reencoded = wyrebox_flag_changed_payload_encode (&decoded, &error);
    g_assert_no_error (error);
    g_assert_true (g_bytes_equal (encoded, reencoded));
}

static void
test_round_trip_each_mode (void)
{
    WyreboxFlagChangedPayload payload = valid_payload ();

    assert_round_trip (&payload);

    payload.mode = WYREBOX_FLAG_CHANGED_MODE_CLEAR;
    payload.user_keywords = NULL;
    assert_round_trip (&payload);

    payload.mode = WYREBOX_FLAG_CHANGED_MODE_REPLACE;
    payload.system_flags = NULL;
    payload.user_keywords = one_keyword;
    assert_round_trip (&payload);
}

static void
test_round_trip_empty_replace (void)
{
    WyreboxFlagChangedPayload payload = valid_payload ();

    payload.mode = WYREBOX_FLAG_CHANGED_MODE_REPLACE;
    payload.system_flags = NULL;
    payload.user_keywords = NULL;
    assert_round_trip (&payload);
}

static void
test_encoded_header_matches_golden (void)
{
    g_autoptr (GBytes) encoded = encode_valid_payload ();
    const guint8 *data = NULL;
    gsize size = 0;
    static const guint8 expected_header[] = {
        'W', 'Y', 'R', 'E', 'F', 'L', 'C', '1',
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00,
        0x02, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x09, 0x00, 0x00, 0x00,
        'a', 'c', 'c', 'o', 'u', 'n', 't', '-', '1',
    };

    data = g_bytes_get_data (encoded, &size);
    g_assert_cmpuint (size, >, sizeof (expected_header));
    g_assert_cmpmem (data, sizeof (expected_header), expected_header,
        sizeof (expected_header));
}

static void
test_encode_rejects_invalid_arguments (void)
{
    static char *unknown_flag[] = { (char *)"\\Recent", NULL };
    static char *duplicate_flags[] =
    { (char *)"\\Seen", (char *)"\\Seen", NULL };
    static char *system_keyword[] = { (char *)"\\Seen", NULL };
    static char *spaced_keyword[] = { (char *)"two words", NULL };
    static char *empty_keyword[] = { (char *)"", NULL };
    static char *duplicate_keywords[] =
    { (char *)"project-x", (char *)"project-x", NULL };
    WyreboxFlagChangedPayload payload = valid_payload ();

    payload.account_id = NULL;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.mailbox_id = (char *)"";
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.uid = 0;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.uidvalidity = 0;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.mode = 3;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.system_flags = unknown_flag;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.system_flags = duplicate_flags;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.user_keywords = system_keyword;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.user_keywords = spaced_keyword;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.user_keywords = empty_keyword;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.user_keywords = duplicate_keywords;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.mode = WYREBOX_FLAG_CHANGED_MODE_CLEAR;
    payload.system_flags = NULL;
    payload.user_keywords = NULL;
    assert_encode_fails (&payload);
}

static void
test_decode_rejects_bad_magic_and_version (void)
{
    g_autoptr (GBytes) encoded = encode_valid_payload ();
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);
    g_autofree guint8 *copy = g_memdup2 (data, size);
    g_autoptr (GBytes) bad_magic = NULL;
    g_autoptr (GBytes) bad_version = NULL;

    copy[0] = 'X';
    bad_magic = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (bad_magic);

    memcpy (copy, data, size);
    copy[7] = '2';
    bad_version = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (bad_version);
}

static void
test_decode_rejects_truncation_and_trailing_bytes (void)
{
    g_autoptr (GBytes) encoded = encode_valid_payload ();
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);
    g_autoptr (GBytes) truncated_header = g_bytes_new (data, HEADER_SIZE - 1);
    g_autoptr (GBytes) truncated_string = g_bytes_new (data, size - 1);
    g_autoptr (GByteArray) array = g_byte_array_new ();
    g_autoptr (GBytes) trailing = NULL;
    const guint8 extra = 0xff;

    assert_decode_fails_invalid_data (truncated_header);
    assert_decode_fails_invalid_data (truncated_string);

    g_byte_array_append (array, data, size);
    g_byte_array_append (array, &extra, sizeof (extra));
    trailing = g_byte_array_free_to_bytes (g_steal_pointer (&array));
    assert_decode_fails_invalid_data (trailing);
}

static void
test_decode_rejects_invalid_fields (void)
{
    g_autoptr (GBytes) encoded = encode_valid_payload ();
    gsize size = 0;
    const guint8 *data = g_bytes_get_data (encoded, &size);
    g_autofree guint8 *copy = g_memdup2 (data, size);
    g_autoptr (GBytes) bad_uid = NULL;
    g_autoptr (GBytes) bad_mode = NULL;
    g_autoptr (GBytes) bad_count = NULL;
    g_autoptr (GBytes) bad_flag = NULL;
    g_autoptr (GBytes) nul_string = NULL;

    memset (copy + 8, 0, sizeof (guint64));
    bad_uid = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (bad_uid);

    memcpy (copy, data, size);
    copy[24] = 3;
    bad_mode = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (bad_mode);

    memcpy (copy, data, size);
    copy[25] = 0xff;
    copy[26] = 0xff;
    copy[27] = 0xff;
    copy[28] = 0xff;
    bad_count = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (bad_count);

    /* account (4 + 9) and mailbox (4 + 15) precede the first flag. */
    memcpy (copy, data, size);
    copy[HEADER_SIZE + 13 + 19 + 4 + 1] = 'X';
    bad_flag = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (bad_flag);

    memcpy (copy, data, size);
    copy[HEADER_SIZE + 4] = '\0';
    nul_string = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (nul_string);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/journal/flag-changed/roundtrip-modes",
        test_round_trip_each_mode);
    g_test_add_func ("/journal/flag-changed/roundtrip-empty-replace",
        test_round_trip_empty_replace);
    g_test_add_func ("/journal/flag-changed/golden-header",
        test_encoded_header_matches_golden);
    g_test_add_func ("/journal/flag-changed/encode-invalid",
        test_encode_rejects_invalid_arguments);
    g_test_add_func ("/journal/flag-changed/decode-magic",
        test_decode_rejects_bad_magic_and_version);
    g_test_add_func ("/journal/flag-changed/decode-truncation",
        test_decode_rejects_truncation_and_trailing_bytes);
    g_test_add_func ("/journal/flag-changed/decode-invalid-fields",
        test_decode_rejects_invalid_fields);

    return g_test_run ();
}
