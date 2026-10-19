#include "wyrebox-memberships-copied-payload.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

static char *sample_flags[] = { (char *)"\\Seen", (char *)"\\Flagged", NULL };
static char *sample_keywords[] = { (char *)"project-x", (char *)"$Junk", NULL };

static void
fill_items (WyreboxMembershipsCopiedItem items[2])
{
    items[0] = (WyreboxMembershipsCopiedItem) {
        .src_uid = 3,
        .dst_uid = 11,
        .internal_date_unix_us = 1700000000000000ULL,
        .message_id = (char *)"message-a",
        .system_flags = sample_flags,
        .user_keywords = sample_keywords,
    };
    items[1] = (WyreboxMembershipsCopiedItem) {
        .src_uid = 8,
        .dst_uid = 12,
        .internal_date_unix_us = 0,
        .message_id = (char *)"message-b",
    };
}

static WyreboxMembershipsCopiedPayload
valid_payload (WyreboxMembershipsCopiedItem *items, gsize n_items)
{
    return (WyreboxMembershipsCopiedPayload) {
               .account_id = (char *)"account-1",
               .source_mailbox_id = (char *)"inbox:account-1",
               .dest_mailbox_id = (char *)"archive:account-1",
               .source_uidvalidity = 1,
               .dest_uidvalidity = 4,
               .items = items,
               .n_items = n_items,
    };
}

static void
assert_encode_fails (const WyreboxMembershipsCopiedPayload *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;

    encoded = wyrebox_memberships_copied_payload_encode (payload, &error);
    g_assert_null (encoded);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_decode_fails_invalid_data (GBytes *bytes)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxMembershipsCopiedPayload) decoded = { 0 };

    g_assert_false (wyrebox_memberships_copied_payload_decode (bytes,
        &decoded, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_null (decoded.account_id);
    g_assert_null (decoded.items);
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
assert_round_trip (const WyreboxMembershipsCopiedPayload *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) reencoded = NULL;
    g_auto (WyreboxMembershipsCopiedPayload) decoded = { 0 };

    encoded = wyrebox_memberships_copied_payload_encode (payload, &error);
    g_assert_no_error (error);
    g_assert_nonnull (encoded);
    g_assert_true (wyrebox_memberships_copied_payload_decode (encoded,
        &decoded, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (decoded.account_id, ==, payload->account_id);
    g_assert_cmpstr (decoded.source_mailbox_id, ==,
        payload->source_mailbox_id);
    g_assert_cmpstr (decoded.dest_mailbox_id, ==, payload->dest_mailbox_id);
    g_assert_cmpuint (decoded.source_uidvalidity, ==,
        payload->source_uidvalidity);
    g_assert_cmpuint (decoded.dest_uidvalidity, ==, payload->dest_uidvalidity);
    g_assert_cmpuint (decoded.n_items, ==, payload->n_items);
    for (gsize i = 0; i < payload->n_items; i++) {
        g_assert_cmpuint (decoded.items[i].src_uid, ==,
            payload->items[i].src_uid);
        g_assert_cmpuint (decoded.items[i].dst_uid, ==,
            payload->items[i].dst_uid);
        g_assert_cmpuint (decoded.items[i].internal_date_unix_us, ==,
            payload->items[i].internal_date_unix_us);
        g_assert_cmpstr (decoded.items[i].message_id, ==,
            payload->items[i].message_id);
        assert_strv_equal (decoded.items[i].system_flags,
            payload->items[i].system_flags);
        assert_strv_equal (decoded.items[i].user_keywords,
            payload->items[i].user_keywords);
    }

    reencoded = wyrebox_memberships_copied_payload_encode (&decoded, &error);
    g_assert_no_error (error);
    g_assert_true (g_bytes_equal (encoded, reencoded));
}

static void
test_round_trip (void)
{
    WyreboxMembershipsCopiedItem items[2];
    WyreboxMembershipsCopiedPayload payload;
    g_autoptr (GBytes) encoded = NULL;

    fill_items (items);
    payload = valid_payload (items, G_N_ELEMENTS (items));
    assert_round_trip (&payload);

    payload.n_items = 1;
    assert_round_trip (&payload);

    encoded = wyrebox_memberships_copied_payload_encode (&payload, NULL);
    g_assert_cmpmem (g_bytes_get_data (encoded, NULL), 8, "WYREMCP1", 8);
}

static void
test_rejects_invalid_encode_inputs (void)
{
    WyreboxMembershipsCopiedItem items[2];
    WyreboxMembershipsCopiedPayload payload;
    char *bad_flags[] = { (char *)"\\Recent", NULL };
    char *duplicate_flags[] = { (char *)"\\Seen", (char *)"\\Seen", NULL };
    char *bad_keywords[] = { (char *)"two words", NULL };
    char *backslash_keywords[] = { (char *)"\\Custom", NULL };

    fill_items (items);
    payload = valid_payload (items, G_N_ELEMENTS (items));
    payload.account_id = NULL;
    assert_encode_fails (&payload);

    payload = valid_payload (items, G_N_ELEMENTS (items));
    payload.source_mailbox_id = (char *)"";
    assert_encode_fails (&payload);

    payload = valid_payload (items, G_N_ELEMENTS (items));
    payload.dest_mailbox_id = NULL;
    assert_encode_fails (&payload);

    payload = valid_payload (items, G_N_ELEMENTS (items));
    payload.source_uidvalidity = 0;
    assert_encode_fails (&payload);

    payload = valid_payload (items, G_N_ELEMENTS (items));
    payload.dest_uidvalidity = 0;
    assert_encode_fails (&payload);

    payload = valid_payload (items, G_N_ELEMENTS (items));
    payload.n_items = 0;
    assert_encode_fails (&payload);

    payload = valid_payload (NULL, 1);
    assert_encode_fails (&payload);

    payload = valid_payload (items, G_N_ELEMENTS (items));
    items[0].src_uid = 0;
    assert_encode_fails (&payload);

    fill_items (items);
    items[1].dst_uid = 0;
    assert_encode_fails (&payload);

    fill_items (items);
    items[1].dst_uid = items[0].dst_uid;
    assert_encode_fails (&payload);

    fill_items (items);
    items[1].message_id = (char *)"";
    assert_encode_fails (&payload);

    fill_items (items);
    items[1].message_id = NULL;
    assert_encode_fails (&payload);

    fill_items (items);
    items[0].system_flags = bad_flags;
    assert_encode_fails (&payload);

    fill_items (items);
    items[0].system_flags = duplicate_flags;
    assert_encode_fails (&payload);

    fill_items (items);
    items[0].user_keywords = bad_keywords;
    assert_encode_fails (&payload);

    fill_items (items);
    items[0].user_keywords = backslash_keywords;
    assert_encode_fails (&payload);
}

static void
test_rejects_truncated_and_bad_magic (void)
{
    WyreboxMembershipsCopiedItem items[2];
    WyreboxMembershipsCopiedPayload payload;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) truncated_header = g_bytes_new_static ("WYREMCP", 7);
    g_autoptr (GBytes) bad = NULL;
    g_autoptr (GBytes) trailing = NULL;
    g_autoptr (GBytes) zero_items = NULL;
    const guint8 *data = NULL;
    gsize size = 0;
    g_autofree guint8 *copy = NULL;

    fill_items (items);
    payload = valid_payload (items, G_N_ELEMENTS (items));
    encoded = wyrebox_memberships_copied_payload_encode (&payload, NULL);
    g_assert_nonnull (encoded);
    data = g_bytes_get_data (encoded, &size);

    assert_decode_fails_invalid_data (truncated_header);

    copy = g_memdup2 (data, size);
    memcpy (copy, "WYREXXXX", 8);
    bad = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (bad);

    for (gsize cut = 0; cut < size; cut++) {
        g_autoptr (GBytes) prefix = g_bytes_new (data, cut);

        assert_decode_fails_invalid_data (prefix);
    }

    g_free (copy);
    copy = g_malloc0 (size + 1);
    memcpy (copy, data, size);
    trailing = g_bytes_new (copy, size + 1);
    assert_decode_fails_invalid_data (trailing);

    memset (copy + 24, 0, 4);
    zero_items = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (zero_items);
}

static void
test_rejects_invalid_decoded_payload (void)
{
    WyreboxMembershipsCopiedItem items[2];
    WyreboxMembershipsCopiedPayload payload;
    g_autoptr (GBytes) encoded = NULL;
    g_autofree guint8 *copy = NULL;
    g_autoptr (GBytes) bad = NULL;
    gsize size = 0;
    const guint8 *data = NULL;

    fill_items (items);
    payload = valid_payload (items, G_N_ELEMENTS (items));
    encoded = wyrebox_memberships_copied_payload_encode (&payload, NULL);
    g_assert_nonnull (encoded);
    data = g_bytes_get_data (encoded, &size);

    /* Zero source UIDVALIDITY survives framing but fails validation. */
    copy = g_memdup2 (data, size);
    memset (copy + 8, 0, 8);
    bad = g_bytes_new (copy, size);
    assert_decode_fails_invalid_data (bad);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/journal/memberships-copied/round-trip",
        test_round_trip);
    g_test_add_func ("/journal/memberships-copied/rejects-invalid-encode",
        test_rejects_invalid_encode_inputs);
    g_test_add_func ("/journal/memberships-copied/rejects-bad-decode",
        test_rejects_truncated_and_bad_magic);
    g_test_add_func ("/journal/memberships-copied/rejects-invalid-decoded",
        test_rejects_invalid_decoded_payload);

    return g_test_run ();
}
