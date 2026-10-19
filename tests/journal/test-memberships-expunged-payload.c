#include "wyrebox-memberships-expunged-payload.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

static guint64 sample_uids[] = { 1, 3, 7 };

static WyreboxMembershipsExpungedPayload
valid_payload (void)
{
    return (WyreboxMembershipsExpungedPayload) {
               .account_id = (char *)"account-1",
               .mailbox_id = (char *)"inbox:account-1",
               .uidvalidity = 1,
               .uids = sample_uids,
               .n_uids = G_N_ELEMENTS (sample_uids),
    };
}

static void
assert_encode_fails (const WyreboxMembershipsExpungedPayload *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;

    encoded = wyrebox_memberships_expunged_payload_encode (payload, &error);
    g_assert_null (encoded);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_decode_fails_invalid_data (GBytes *bytes)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxMembershipsExpungedPayload) decoded = { 0 };

    g_assert_false (wyrebox_memberships_expunged_payload_decode (bytes,
        &decoded, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_null (decoded.account_id);
}

static void
assert_round_trip (const WyreboxMembershipsExpungedPayload *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) reencoded = NULL;
    g_auto (WyreboxMembershipsExpungedPayload) decoded = { 0 };

    encoded = wyrebox_memberships_expunged_payload_encode (payload, &error);
    g_assert_no_error (error);
    g_assert_nonnull (encoded);
    g_assert_true (wyrebox_memberships_expunged_payload_decode (encoded,
        &decoded, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (decoded.account_id, ==, payload->account_id);
    g_assert_cmpstr (decoded.mailbox_id, ==, payload->mailbox_id);
    g_assert_cmpuint (decoded.uidvalidity, ==, payload->uidvalidity);
    g_assert_cmpuint (decoded.n_uids, ==, payload->n_uids);
    for (gsize i = 0; i < payload->n_uids; i++)
        g_assert_cmpuint (decoded.uids[i], ==, payload->uids[i]);

    reencoded = wyrebox_memberships_expunged_payload_encode (&decoded, &error);
    g_assert_no_error (error);
    g_assert_true (g_bytes_equal (encoded, reencoded));
}

static void
test_round_trip (void)
{
    WyreboxMembershipsExpungedPayload payload = valid_payload ();
    guint64 one_uid[] = { 9 };

    assert_round_trip (&payload);

    payload.uids = one_uid;
    payload.n_uids = 1;
    assert_round_trip (&payload);
}

static void
test_rejects_invalid_encode_inputs (void)
{
    WyreboxMembershipsExpungedPayload payload = valid_payload ();
    guint64 unordered[] = { 3, 1 };
    guint64 zero_uid[] = { 0 };

    payload.account_id = NULL;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.mailbox_id = (char *)"";
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.uidvalidity = 0;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.n_uids = 0;
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.uids = unordered;
    payload.n_uids = G_N_ELEMENTS (unordered);
    assert_encode_fails (&payload);

    payload = valid_payload ();
    payload.uids = zero_uid;
    payload.n_uids = 1;
    assert_encode_fails (&payload);
}

static void
test_rejects_truncated_and_bad_magic (void)
{
    g_autoptr (GBytes) truncated = g_bytes_new_static ("WYREMEX", 7);
    guint8 bad_magic[20] = { 0 };
    g_autoptr (GBytes) bad = NULL;

    memcpy (bad_magic, "WYREXXXX", 8);
    assert_decode_fails_invalid_data (truncated);
    bad = g_bytes_new (bad_magic, sizeof (bad_magic));
    assert_decode_fails_invalid_data (bad);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/journal/memberships-expunged/round-trip",
        test_round_trip);
    g_test_add_func ("/journal/memberships-expunged/rejects-invalid-encode",
        test_rejects_invalid_encode_inputs);
    g_test_add_func ("/journal/memberships-expunged/rejects-bad-decode",
        test_rejects_truncated_and_bad_magic);

    return g_test_run ();
}
