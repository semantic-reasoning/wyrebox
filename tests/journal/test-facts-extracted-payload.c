#include "wyrebox-facts-extracted-payload.h"

#include <gio/gio.h>
#include <string.h>

static WyreboxFactRecord *
new_fact (const char *predicate, const char *source, const char *first,
    const char *second)
{
    const char *args[] = { first, second, NULL };
    WyreboxFactRecord *fact = g_new0 (WyreboxFactRecord, 1);
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_fact_record_init (fact, predicate, args, source,
        1000000, 7, &error));
    g_assert_no_error (error);
    return fact;
}

static void
init_payload (WyreboxFactsExtractedPayload *payload)
{
    payload->account_id = g_strdup ("account-1");
    payload->message_id = g_strdup ("journal:0:1");
    payload->extracted_at_unix_us = 1234;
    payload->facts = g_ptr_array_new_with_free_func
            (wyrebox_facts_extracted_payload_fact_free);
    g_ptr_array_add (payload->facts, new_fact ("message_id",
        "header:message-id", "journal:0:1", "<root@example.test>"));
    g_ptr_array_add (payload->facts, new_fact ("participant", "header:to",
        "journal:0:1", "Ann \"Lead\" <ann@example.test>"));
}

static void
test_round_trip (void)
{
    g_auto (WyreboxFactsExtractedPayload) payload = { 0 };
    g_auto (WyreboxFactsExtractedPayload) decoded = { 0 };
    g_autoptr (GBytes) bytes = NULL;
    g_autoptr (GError) error = NULL;
    const WyreboxFactRecord *fact = NULL;

    init_payload (&payload);
    bytes = wyrebox_facts_extracted_payload_encode (&payload, &error);
    g_assert_no_error (error);
    g_assert_nonnull (bytes);

    g_assert_true (wyrebox_facts_extracted_payload_decode (bytes, &decoded,
        &error));
    g_assert_no_error (error);
    g_assert_cmpstr (decoded.account_id, ==, "account-1");
    g_assert_cmpstr (decoded.message_id, ==, "journal:0:1");
    g_assert_cmpuint (decoded.extracted_at_unix_us, ==, 1234);
    g_assert_cmpuint (decoded.facts->len, ==, 2);

    fact = g_ptr_array_index (decoded.facts, 1);
    g_assert_cmpstr (fact->predicate, ==, "participant");
    g_assert_cmpstr (fact->source, ==, "header:to");
    g_assert_cmpuint (fact->confidence_ppm, ==, 1000000);
    g_assert_cmpuint (fact->created_at_unix_us, ==, 1234);
    g_assert_cmpuint (g_strv_length (fact->args), ==, 2);
    g_assert_cmpstr (fact->args[1], ==, "Ann \"Lead\" <ann@example.test>");
}

static void
test_round_trip_without_facts (void)
{
    g_auto (WyreboxFactsExtractedPayload) payload = { 0 };
    g_auto (WyreboxFactsExtractedPayload) decoded = { 0 };
    g_autoptr (GBytes) bytes = NULL;
    g_autoptr (GError) error = NULL;

    payload.account_id = g_strdup ("account-1");
    payload.message_id = g_strdup ("journal:0:1");
    payload.extracted_at_unix_us = 1;

    bytes = wyrebox_facts_extracted_payload_encode (&payload, &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_facts_extracted_payload_decode (bytes, &decoded,
        &error));
    g_assert_no_error (error);
    g_assert_nonnull (decoded.facts);
    g_assert_cmpuint (decoded.facts->len, ==, 0);
}

static void
test_rejects_facts_about_other_messages (void)
{
    g_auto (WyreboxFactsExtractedPayload) payload = { 0 };
    g_autoptr (GBytes) bytes = NULL;
    g_autoptr (GError) error = NULL;

    init_payload (&payload);
    g_ptr_array_add (payload.facts, new_fact ("sender_domain", "header:from",
        "journal:0:2", "example.test"));

    bytes = wyrebox_facts_extracted_payload_encode (&payload, &error);
    g_assert_null (bytes);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_rejects_malformed_bytes (void)
{
    g_auto (WyreboxFactsExtractedPayload) payload = { 0 };
    g_auto (WyreboxFactsExtractedPayload) decoded = { 0 };
    g_autoptr (GBytes) bytes = NULL;
    g_autoptr (GBytes) truncated = NULL;
    g_autoptr (GBytes) bad_magic = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree guint8 *copy = NULL;
    gsize size = 0;

    init_payload (&payload);
    bytes = wyrebox_facts_extracted_payload_encode (&payload, &error);
    g_assert_no_error (error);
    size = g_bytes_get_size (bytes);

    truncated = g_bytes_new_from_bytes (bytes, 0, size - 1);
    g_assert_false (wyrebox_facts_extracted_payload_decode (truncated,
        &decoded, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);

    copy = g_memdup2 (g_bytes_get_data (bytes, NULL), size);
    copy[0] = 'X';
    bad_magic = g_bytes_new (copy, size);
    g_assert_false (wyrebox_facts_extracted_payload_decode (bad_magic,
        &decoded, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/journal/facts-extracted-payload/round-trip",
        test_round_trip);
    g_test_add_func (
        "/journal/facts-extracted-payload/round-trip-without-facts",
        test_round_trip_without_facts);
    g_test_add_func
        ("/journal/facts-extracted-payload/rejects-facts-about-other-messages",
        test_rejects_facts_about_other_messages);
    g_test_add_func ("/journal/facts-extracted-payload/rejects-malformed-bytes",
        test_rejects_malformed_bytes);

    return g_test_run ();
}
