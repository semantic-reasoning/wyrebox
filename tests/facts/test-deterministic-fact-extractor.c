#include "wyrebox-deterministic-fact-extractor.h"

#include <gio/gio.h>
#include <glib.h>

static const WyreboxFactRecord *
fact_at (GPtrArray *facts, guint index)
{
  g_assert_nonnull (facts);
  g_assert_cmpuint (facts->len, >, index);

  return g_ptr_array_index (facts, index);
}

static void
assert_fact (const WyreboxFactRecord *fact,
    const char *predicate,
    const char *arg0, const char *arg1, const char *source)
{
  g_assert_cmpstr (fact->predicate, ==, predicate);
  g_assert_cmpstr (fact->args[0], ==, arg0);
  g_assert_cmpstr (fact->args[1], ==, arg1);
  g_assert_null (fact->args[2]);
  g_assert_cmpstr (fact->source, ==, source);
  g_assert_cmpuint (fact->confidence_ppm, ==, 1000000);
  g_assert_cmpuint (fact->created_at_unix_us, ==, 1800000000000000);
  g_assert_cmpuint (fact->retracted_at_unix_us, ==, 0);
}

static void
test_extracts_header_facts_from_metadata (void)
{
  WyreboxEmlMetadata metadata = {
    .message_id = "<mail-1@example.test>",
    .from = "Alice <alice@Example.TEST>",
    .to = "Bob <bob@example.test>",
    .cc = "Carol <carol@example.test>",
    .date = "Tue, 02 Jun 2026 12:34:56 +0000",
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) facts = NULL;

  facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-1",
      &metadata, 1800000000000000, &error);
  g_assert_no_error (error);
  g_assert_nonnull (facts);
  g_assert_cmpuint (facts->len, ==, 6);

  assert_fact (fact_at (facts, 0),
      "message_id", "mail-1", "<mail-1@example.test>", "header:message-id");
  assert_fact (fact_at (facts, 1),
      "sender_domain", "mail-1", "example.test", "header:from");
  assert_fact (fact_at (facts, 2),
      "participant", "mail-1", "Alice <alice@Example.TEST>", "header:from");
  assert_fact (fact_at (facts, 3),
      "participant", "mail-1", "Bob <bob@example.test>", "header:to");
  assert_fact (fact_at (facts, 4),
      "participant", "mail-1", "Carol <carol@example.test>", "header:cc");
  assert_fact (fact_at (facts, 5),
      "sent_at", "mail-1", "Tue, 02 Jun 2026 12:34:56 +0000", "header:date");
}

static void
test_extracts_only_present_header_facts (void)
{
  WyreboxEmlMetadata metadata = {
    .from = "no-domain-address",
    .bcc = "Hidden <hidden@example.test>",
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) facts = NULL;

  facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-2",
      &metadata, 1800000000000000, &error);
  g_assert_no_error (error);
  g_assert_nonnull (facts);
  g_assert_cmpuint (facts->len, ==, 2);

  assert_fact (fact_at (facts, 0),
      "participant", "mail-2", "no-domain-address", "header:from");
  assert_fact (fact_at (facts, 1),
      "participant", "mail-2", "Hidden <hidden@example.test>", "header:bcc");
}

static void
test_extracts_message_reference_facts (void)
{
  WyreboxEmlMetadata metadata = {
    .in_reply_to = "<parent@example.test>",
    .references = "<root@example.test> <parent@example.test>",
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) facts = NULL;

  facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-3",
      &metadata, 1800000000000000, &error);
  g_assert_no_error (error);
  g_assert_nonnull (facts);
  g_assert_cmpuint (facts->len, ==, 3);

  assert_fact (fact_at (facts, 0),
      "replies_to", "mail-3", "<parent@example.test>", "header:in-reply-to");
  assert_fact (fact_at (facts, 1),
      "references", "mail-3", "<root@example.test>", "header:references");
  assert_fact (fact_at (facts, 2),
      "references", "mail-3", "<parent@example.test>", "header:references");
}

static void
test_ignores_reference_headers_without_message_id_tokens (void)
{
  WyreboxEmlMetadata metadata = {
    .in_reply_to = "not a message id",
    .references = "also not ids",
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) facts = NULL;

  facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-4",
      &metadata, 1800000000000000, &error);
  g_assert_no_error (error);
  g_assert_nonnull (facts);
  g_assert_cmpuint (facts->len, ==, 0);
}

static void
test_rejects_missing_mail_id (void)
{
  WyreboxEmlMetadata metadata = {
    .from = "Alice <alice@example.test>",
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) facts = NULL;

  facts = wyrebox_deterministic_fact_extract_from_metadata ("",
      &metadata, 1800000000000000, &error);
  g_assert_null (facts);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_rejects_missing_created_timestamp (void)
{
  WyreboxEmlMetadata metadata = {
    .from = "Alice <alice@example.test>",
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) facts = NULL;

  facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-3",
      &metadata, 0, &error);
  g_assert_null (facts);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/facts/deterministic-extractor/header-facts",
      test_extracts_header_facts_from_metadata);
  g_test_add_func ("/facts/deterministic-extractor/only-present-header-facts",
      test_extracts_only_present_header_facts);
  g_test_add_func ("/facts/deterministic-extractor/message-reference-facts",
      test_extracts_message_reference_facts);
  g_test_add_func ("/facts/deterministic-extractor/"
      "ignores-reference-headers-without-message-id-tokens",
      test_ignores_reference_headers_without_message_id_tokens);
  g_test_add_func ("/facts/deterministic-extractor/rejects-missing-mail-id",
      test_rejects_missing_mail_id);
  g_test_add_func ("/facts/deterministic-extractor/rejects-missing-created-at",
      test_rejects_missing_created_timestamp);

  return g_test_run ();
}
