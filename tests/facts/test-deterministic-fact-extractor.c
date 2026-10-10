#include "wyrebox-deterministic-fact-extractor.h"

#include <string.h>

#include <gio/gio.h>
#include <glib.h>

static GBytes *
load_fixture_bytes (const char *fixture_dir, const char *name)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *path = g_build_filename (fixture_dir, name, NULL);
    g_autofree char *contents = NULL;
    gsize length = 0;

    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    g_assert_no_error (error);

    return g_bytes_new_take (g_steal_pointer (&contents), length);
}

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
assert_fact_with_display_name (const WyreboxFactRecord *fact,
    const char *mail_id,
    const char *address,
    const char *display_name,
    const char *source)
{
    g_assert_cmpstr (fact->predicate, ==, "participant_display_name");
    g_assert_cmpstr (fact->args[0], ==, mail_id);
    g_assert_cmpstr (fact->args[1], ==, address);
    g_assert_cmpstr (fact->args[2], ==, display_name);
    g_assert_null (fact->args[3]);
    g_assert_cmpstr (fact->source, ==, source);
}

static void
test_extracts_decoded_and_normalized_header_facts (void)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    const WyreboxDeterministicFactDictionaryRule rules[] = {
        {
            .field = "subject",
            .rule_id = "korean-subject",
            .match_text = "회의",
            .canonical_project_key = "project-meeting",
        },
    };
    const WyreboxDeterministicFactRegexRule regex_rules[] = {
        {
            .field = "subject",
            .rule_id = "decoded-meeting",
            .predicate = "reference_candidate",
            .pattern = "회의",
            .capture_group = 0,
        },
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) bytes = NULL;
    g_auto (WyreboxEmlMetadata) metadata = { 0 };
    g_autoptr (GPtrArray) facts = NULL;
    g_autoptr (GPtrArray) repeated_facts = NULL;
    g_autofree char *wirelog = NULL;
    g_autofree char *repeated_wirelog = NULL;

    g_assert_nonnull (fixture_dir);
    bytes = load_fixture_bytes (fixture_dir,
            "deterministic-header-normalization.eml");
    g_assert_true (wyrebox_eml_metadata_parse_bytes (bytes, &metadata, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (metadata.subject, ==,
        "=?UTF-8?B?7ZqM?= =?UTF-8?Q?=EC=9D=98?=");
    g_assert_cmpstr (metadata.from, ==,
        "=?UTF-8?Q?=ED=99=8D_Hong?= (team) "
        "<Sender@EXAMPLE.TEST> (account)");

    facts = wyrebox_deterministic_fact_extract_from_metadata_with_rules
            ("mail-normalized", &metadata, 1800000000000000, rules,
            G_N_ELEMENTS (rules), regex_rules, G_N_ELEMENTS (regex_rules),
            &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 18);
    g_assert_cmpstr (metadata.subject, ==,
        "=?UTF-8?B?7ZqM?= =?UTF-8?Q?=EC=9D=98?=");
    g_assert_cmpstr (metadata.from, ==,
        "=?UTF-8?Q?=ED=99=8D_Hong?= (team) "
        "<Sender@EXAMPLE.TEST> (account)");

    assert_fact (fact_at (facts, 0),
        "message_id", "mail-normalized",
        "<deterministic-header-normalization@example.test>",
        "header:message-id");
    assert_fact (fact_at (facts, 1),
        "sender_domain", "mail-normalized", "example.test", "header:from");
    assert_fact (fact_at (facts, 2),
        "participant", "mail-normalized", "Sender@example.test",
        "header:from");
    assert_fact_with_display_name (fact_at (facts, 3),
        "mail-normalized", "Sender@example.test", "홍 Hong", "header:from");
    assert_fact (fact_at (facts, 4),
        "participant", "mail-normalized", "Alice@example.test", "header:to");
    assert_fact_with_display_name (fact_at (facts, 5),
        "mail-normalized", "Alice@example.test", "Doe, Alice", "header:to");
    assert_fact (fact_at (facts, 6),
        "participant", "mail-normalized", "Bob@example.test", "header:to");
    assert_fact_with_display_name (fact_at (facts, 7),
        "mail-normalized", "Bob@example.test", "Bob", "header:to");
    assert_fact (fact_at (facts, 8),
        "participant", "mail-normalized", "Zoe@example.test", "header:cc");
    assert_fact_with_display_name (fact_at (facts, 9),
        "mail-normalized", "Zoe@example.test", "Zoë, Reader (alias)",
        "header:cc");
    assert_fact (fact_at (facts, 10),
        "participant", "mail-normalized", "other@example.test", "header:cc");
    assert_fact (fact_at (facts, 11),
        "participant", "mail-normalized", "Blind@example.test",
        "header:bcc");
    assert_fact_with_display_name (fact_at (facts, 12),
        "mail-normalized", "Blind@example.test",
        "=?X-UNKNOWN?B?Tm9tZQ==?=", "header:bcc");
    assert_fact (fact_at (facts, 13),
        "sent_at", "mail-normalized", "Tue, 02 Jun 2026 12:34:56 +0000",
        "header:date");
    assert_fact (fact_at (facts, 14),
        "list_id", "mail-normalized", "updates.wyrebox.example",
        "header:list-id");
    assert_fact (fact_at (facts, 15),
        "delivered_to", "mail-normalized", "Inbox+Tag@example.test",
        "header:delivered-to");
    assert_fact (fact_at (facts, 16),
        "project_keyword", "mail-normalized", "project-meeting",
        "dictionary:subject:korean-subject");
    assert_fact (fact_at (facts, 17),
        "reference_candidate", "mail-normalized", "회의",
        "regex:subject:decoded-meeting");

    repeated_facts = wyrebox_deterministic_fact_extract_from_metadata_with_rules
            ("mail-normalized", &metadata, 1800000000000000, rules,
            G_N_ELEMENTS (rules), regex_rules, G_N_ELEMENTS (regex_rules),
            &error);
    g_assert_no_error (error);
    wirelog = wyrebox_fact_record_array_to_wirelog_facts (facts, &error);
    g_assert_no_error (error);
    repeated_wirelog =
        wyrebox_fact_record_array_to_wirelog_facts (repeated_facts, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (repeated_wirelog, ==, wirelog);
}

static void
test_undecodable_encoded_word_falls_back_to_raw_value (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "=?UTF-8?B?@@@?=",
    };
    const WyreboxDeterministicFactDictionaryRule rule = {
        .field = "subject",
        .rule_id = "raw-encoded-fallback",
        .match_text = "=?UTF-8?B?@@@?=",
        .canonical_project_key = "raw-fallback",
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_dictionary
            ("mail-raw-fallback", &metadata, 1800000000000000, &rule, 1,
            &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 1);
    assert_fact (fact_at (facts, 0),
        "project_keyword", "mail-raw-fallback", "raw-fallback",
        "dictionary:subject:raw-encoded-fallback");

    {
        const WyreboxDeterministicFactDictionaryRule q_rule = {
            .field = "subject",
            .rule_id = "raw-q-fallback",
            .match_text = "=?UTF-8?Q?Hello World?=",
            .canonical_project_key = "raw-fallback",
        };

        metadata.subject = "=?UTF-8?Q?Hello World?=";
        g_clear_pointer (&facts, g_ptr_array_unref);
        facts =
            wyrebox_deterministic_fact_extract_from_metadata_with_dictionary
                ("mail-raw-fallback", &metadata, 1800000000000000, &q_rule, 1,
                &error);
        g_assert_no_error (error);
        g_assert_nonnull (facts);
        g_assert_cmpuint (facts->len, ==, 1);
        assert_fact (fact_at (facts, 0),
            "project_keyword", "mail-raw-fallback", "raw-fallback",
            "dictionary:subject:raw-q-fallback");
    }

    {
        const WyreboxDeterministicFactDictionaryRule base64_rule = {
            .field = "subject",
            .rule_id = "raw-base64-fallback",
            .match_text = "=?UTF-8?B?Zh==?=",
            .canonical_project_key = "raw-fallback",
        };

        metadata.subject = "=?UTF-8?B?Zh==?=";
        g_clear_pointer (&facts, g_ptr_array_unref);
        facts =
            wyrebox_deterministic_fact_extract_from_metadata_with_dictionary
                ("mail-raw-fallback", &metadata, 1800000000000000,
                &base64_rule, 1, &error);
        g_assert_no_error (error);
        g_assert_nonnull (facts);
        g_assert_cmpuint (facts->len, ==, 1);
        assert_fact (fact_at (facts, 0),
            "project_keyword", "mail-raw-fallback", "raw-fallback",
            "dictionary:subject:raw-base64-fallback");
    }
}

static void
test_delivered_to_falls_back_to_x_original_to (void)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) bytes = NULL;
    g_auto (WyreboxEmlMetadata) metadata = { 0 };
    g_autoptr (GPtrArray) facts = NULL;

    g_assert_nonnull (fixture_dir);
    bytes = load_fixture_bytes (fixture_dir,
            "deterministic-header-normalization-fallback.eml");
    g_assert_true (wyrebox_eml_metadata_parse_bytes (bytes, &metadata, &error));
    g_assert_no_error (error);

    facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-fallback",
            &metadata, 1800000000000000, &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 3);
    assert_fact (fact_at (facts, 0),
        "sender_domain", "mail-fallback", "example.test", "header:from");
    assert_fact (fact_at (facts, 1),
        "participant", "mail-fallback", "sender@example.test", "header:from");
    assert_fact (fact_at (facts, 2),
        "delivered_to", "mail-fallback", "Mailbox@example.test",
        "header:x-original-to");
}

static void
test_sender_domain_uses_first_valid_from_address (void)
{
    WyreboxEmlMetadata metadata = {
        .from = "invalid-address, a@@b, a..b@invalid.test, "
            "a(note)b@example.test, "
            "Broken <broken@example.test> trailing, "
            "source@example.test <real@example.test>, "
            "First@EXAMPLE.TEST, second@elsewhere.test",
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-senders",
            &metadata, 1800000000000000, &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 3);
    assert_fact (fact_at (facts, 0),
        "sender_domain", "mail-senders", "example.test", "header:from");
    assert_fact (fact_at (facts, 1),
        "participant", "mail-senders", "First@example.test", "header:from");
    assert_fact (fact_at (facts, 2),
        "participant", "mail-senders", "second@elsewhere.test",
        "header:from");
}

static void
test_unicode_domains_are_lowercased (void)
{
    WyreboxEmlMetadata metadata = {
        .from = "User@BÜCHER.Example",
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-idn",
            &metadata, 1800000000000000, &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 2);
    assert_fact (fact_at (facts, 0),
        "sender_domain", "mail-idn", "bücher.example", "header:from");
    assert_fact (fact_at (facts, 1),
        "participant", "mail-idn", "User@bücher.example", "header:from");
}

static void
test_unterminated_comment_rejects_address_member (void)
{
    WyreboxEmlMetadata metadata = {
        .from = "valid@example.test, invalid@example.test (unfinished",
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-comment",
            &metadata, 1800000000000000, &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 2);
    assert_fact (fact_at (facts, 0),
        "sender_domain", "mail-comment", "example.test", "header:from");
    assert_fact (fact_at (facts, 1),
        "participant", "mail-comment", "valid@example.test", "header:from");
}

static void
test_invalid_utf8_local_part_is_ignored (void)
{
    char raw_from[] = {
        'b', 'a', 'd', (char)0xff, '@', 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        '.', 't', 'e', 's', 't', '\0',
    };
    WyreboxEmlMetadata metadata = {
        .from = raw_from,
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-invalid",
            &metadata, 1800000000000000, &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 0);
}

static void
test_groups_with_quoted_or_commented_markers (void)
{
    WyreboxEmlMetadata metadata = {
        .to = "\"Ops@Cloud\": Alice <alice@example.test>;, "
            "Ops (<Cloud>): Bob <bob@example.test>;, "
            "Ops@Cloud: Carol <carol@example.test>;",
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts = wyrebox_deterministic_fact_extract_from_metadata ("mail-groups",
            &metadata, 1800000000000000, &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 6);
    assert_fact (fact_at (facts, 0),
        "participant", "mail-groups", "alice@example.test", "header:to");
    assert_fact_with_display_name (fact_at (facts, 1),
        "mail-groups", "alice@example.test", "Alice", "header:to");
    assert_fact (fact_at (facts, 2),
        "participant", "mail-groups", "bob@example.test", "header:to");
    assert_fact_with_display_name (fact_at (facts, 3),
        "mail-groups", "bob@example.test", "Bob", "header:to");
    assert_fact (fact_at (facts, 4),
        "participant", "mail-groups", "carol@example.test", "header:to");
    assert_fact_with_display_name (fact_at (facts, 5),
        "mail-groups", "carol@example.test", "Carol", "header:to");
}

static void
test_extracts_header_facts_from_metadata (void)
{
    WyreboxEmlMetadata metadata = {
        .message_id = "<mail-1@example.test>",
        .from = "Alice A. <alice@Example.TEST>",
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
    g_assert_cmpuint (facts->len, ==, 9);

    assert_fact (fact_at (facts, 0),
        "message_id", "mail-1", "<mail-1@example.test>", "header:message-id");
    assert_fact (fact_at (facts, 1),
        "sender_domain", "mail-1", "example.test", "header:from");
    assert_fact (fact_at (facts, 2),
        "participant", "mail-1", "alice@example.test", "header:from");
    assert_fact_with_display_name (fact_at (facts, 3),
        "mail-1", "alice@example.test", "Alice A.", "header:from");
    assert_fact (fact_at (facts, 4),
        "participant", "mail-1", "bob@example.test", "header:to");
    assert_fact_with_display_name (fact_at (facts, 5),
        "mail-1", "bob@example.test", "Bob", "header:to");
    assert_fact (fact_at (facts, 6),
        "participant", "mail-1", "carol@example.test", "header:cc");
    assert_fact_with_display_name (fact_at (facts, 7),
        "mail-1", "carol@example.test", "Carol", "header:cc");
    assert_fact (fact_at (facts, 8),
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
        "participant", "mail-2", "hidden@example.test", "header:bcc");
    assert_fact_with_display_name (fact_at (facts, 1),
        "mail-2", "hidden@example.test", "Hidden", "header:bcc");
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
test_appends_dictionary_project_keywords_after_header_facts (void)
{
    WyreboxEmlMetadata metadata = {
        .message_id = "<mail-dict@example.test>",
        .subject = "Quarterly ALPHA planning",
        .from = "Program Lead <lead@Example.TEST>",
        .to = "Team Bravo <bravo@example.test>",
        .cc = "Ops Desk <ops@example.test>",
        .bcc = "Audit <audit@example.test>",
        .date = "Tue, 02 Jun 2026 12:34:56 +0000",
    };
    const WyreboxDeterministicFactDictionaryRule rules[] = {
        {
            .field = "to",
            .rule_id = "to-bravo",
            .match_text = "BRAVO",
            .canonical_project_key = "project-bravo",
        },
        {
            .field = "subject",
            .rule_id = "subject-alpha",
            .match_text = "alpha",
            .canonical_project_key = "project-alpha",
        },
        {
            .field = "from",
            .rule_id = "from-lead-domain",
            .match_text = "example.test",
            .canonical_project_key = "project-lead",
        },
        {
            .field = "cc",
            .rule_id = "cc-ops",
            .match_text = "OPS DESK",
            .canonical_project_key = "project-ops",
        },
        {
            .field = "bcc",
            .rule_id = "bcc-audit",
            .match_text = "audit",
            .canonical_project_key = "project-hidden",
        },
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_dictionary
            ("mail-dict", &metadata, 1800000000000000, rules,
            G_N_ELEMENTS (rules),
            &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 16);

    assert_fact (fact_at (facts, 0),
        "message_id", "mail-dict", "<mail-dict@example.test>",
        "header:message-id");
    assert_fact (fact_at (facts, 11),
        "project_keyword", "mail-dict", "project-bravo",
        "dictionary:to:to-bravo");
    assert_fact (fact_at (facts, 12),
        "project_keyword", "mail-dict", "project-alpha",
        "dictionary:subject:subject-alpha");
    assert_fact (fact_at (facts, 13),
        "project_keyword", "mail-dict", "project-lead",
        "dictionary:from:from-lead-domain");
    assert_fact (fact_at (facts, 14),
        "project_keyword", "mail-dict", "project-ops", "dictionary:cc:cc-ops");
    assert_fact (fact_at (facts, 15),
        "project_keyword", "mail-dict", "project-hidden",
        "dictionary:bcc:bcc-audit");
}

static void
test_dictionary_rules_match_only_selected_field (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "Project Alpha",
        .from = "alpha@example.test",
    };
    const WyreboxDeterministicFactDictionaryRule rules[] = {
        {
            .field = "to",
            .rule_id = "to-alpha",
            .match_text = "alpha",
            .canonical_project_key = "project-alpha",
        },
        {
            .field = "subject",
            .rule_id = "subject-alpha",
            .match_text = "alpha",
            .canonical_project_key = "project-alpha",
        },
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_dictionary
            ("mail-selected-field", &metadata, 1800000000000000, rules,
            G_N_ELEMENTS (rules), &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 3);

    assert_fact (fact_at (facts, 2),
        "project_keyword", "mail-selected-field", "project-alpha",
        "dictionary:subject:subject-alpha");
}

static void
test_rejects_invalid_dictionary_rules (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "Project Alpha",
    };
    const WyreboxDeterministicFactDictionaryRule invalid_rules[] = {
        {
            .field = "subject",
            .rule_id = "",
            .match_text = "alpha",
            .canonical_project_key = "project-alpha",
        },
        {
            .field = "date",
            .rule_id = "unsupported-field",
            .match_text = "alpha",
            .canonical_project_key = "project-alpha",
        },
        {
            .field = "subject",
            .rule_id = "empty-match",
            .match_text = "",
            .canonical_project_key = "project-alpha",
        },
        {
            .field = "subject",
            .rule_id = "empty-project",
            .match_text = "alpha",
            .canonical_project_key = "",
        },
    };

    for (guint i = 0; i < G_N_ELEMENTS (invalid_rules); i++) {
        g_autoptr (GError) error = NULL;
        g_autoptr (GPtrArray) facts = NULL;

        facts =
            wyrebox_deterministic_fact_extract_from_metadata_with_dictionary
                ("mail-invalid-rule", &metadata, 1800000000000000,
                &invalid_rules[i], 1,
                &error);
        g_assert_null (facts);
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    }
}

static void
test_extracts_subject_amount_candidate_from_capture_group (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "Invoice total USD 1234.56 due",
    };
    const WyreboxDeterministicFactRegexRule rules[] = {
        {
            .field = "subject",
            .rule_id = "subject-usd-amount",
            .predicate = "amount_candidate",
            .pattern = "USD[ ]+([0-9]+[.][0-9]{2})",
            .capture_group = 1,
        },
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_regex
            ("mail-regex-amount", &metadata, 1800000000000000, rules,
            G_N_ELEMENTS (rules), &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 1);

    assert_fact (fact_at (facts, 0),
        "amount_candidate", "mail-regex-amount", "1234.56",
        "regex:subject:subject-usd-amount");
}

static void
test_extracts_subject_reference_candidate_from_full_match (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "Re: Project handoff REF-2026-0042 ready",
    };
    const WyreboxDeterministicFactRegexRule rules[] = {
        {
            .field = "subject",
            .rule_id = "subject-reference",
            .predicate = "reference_candidate",
            .pattern = "REF-[0-9]{4}-[0-9]{4}",
            .capture_group = 0,
        },
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_regex
            ("mail-regex-reference", &metadata, 1800000000000000, rules,
            G_N_ELEMENTS (rules), &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 1);

    assert_fact (fact_at (facts, 0),
        "reference_candidate", "mail-regex-reference", "REF-2026-0042",
        "regex:subject:subject-reference");
}

static void
test_no_match_and_empty_capture_emit_no_regex_facts (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "No candidate here",
    };
    const WyreboxDeterministicFactRegexRule rules[] = {
        {
            .field = "subject",
            .rule_id = "subject-miss",
            .predicate = "amount_candidate",
            .pattern = "USD[ ]+([0-9]+)",
            .capture_group = 1,
        },
        {
            .field = "subject",
            .rule_id = "empty-capture",
            .predicate = "reference_candidate",
            .pattern = "(No)(x*) candidate",
            .capture_group = 2,
        },
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_regex
            ("mail-regex-none", &metadata, 1800000000000000, rules,
            G_N_ELEMENTS (rules), &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 0);
}

static void
test_regex_rules_emit_in_rule_order_then_match_order (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "Refs ABC-001 and ABC-002 cost USD 10 and USD 20",
    };
    const WyreboxDeterministicFactRegexRule rules[] = {
        {
            .field = "subject",
            .rule_id = "amounts",
            .predicate = "amount_candidate",
            .pattern = "USD[ ]+([0-9]+)",
            .capture_group = 1,
        },
        {
            .field = "subject",
            .rule_id = "references",
            .predicate = "reference_candidate",
            .pattern = "ABC-[0-9]+",
            .capture_group = 0,
        },
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_regex
            ("mail-regex-order", &metadata, 1800000000000000, rules,
            G_N_ELEMENTS (rules), &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 4);

    assert_fact (fact_at (facts, 0),
        "amount_candidate", "mail-regex-order", "10", "regex:subject:amounts");
    assert_fact (fact_at (facts, 1),
        "amount_candidate", "mail-regex-order", "20", "regex:subject:amounts");
    assert_fact (fact_at (facts, 2),
        "reference_candidate", "mail-regex-order", "ABC-001",
        "regex:subject:references");
    assert_fact (fact_at (facts, 3),
        "reference_candidate", "mail-regex-order", "ABC-002",
        "regex:subject:references");
}

static void
test_regex_facts_append_after_header_and_dictionary_facts (void)
{
    WyreboxEmlMetadata metadata = {
        .message_id = "<mail-combined@example.test>",
        .subject = "Project Alpha invoice USD 42",
        .from = "Sender <sender@example.test>",
    };
    const WyreboxDeterministicFactDictionaryRule dictionary_rules[] = {
        {
            .field = "subject",
            .rule_id = "project-alpha",
            .match_text = "alpha",
            .canonical_project_key = "project-alpha",
        },
    };
    const WyreboxDeterministicFactRegexRule regex_rules[] = {
        {
            .field = "subject",
            .rule_id = "subject-amount",
            .predicate = "amount_candidate",
            .pattern = "USD[ ]+([0-9]+)",
            .capture_group = 1,
        },
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_rules
            ("mail-combined", &metadata, 1800000000000000, dictionary_rules,
            G_N_ELEMENTS (dictionary_rules), regex_rules,
            G_N_ELEMENTS (regex_rules),
            &error);
    g_assert_no_error (error);
    g_assert_nonnull (facts);
    g_assert_cmpuint (facts->len, ==, 6);

    assert_fact (fact_at (facts, 0),
        "message_id", "mail-combined", "<mail-combined@example.test>",
        "header:message-id");
    assert_fact (fact_at (facts, 4),
        "project_keyword", "mail-combined", "project-alpha",
        "dictionary:subject:project-alpha");
    assert_fact (fact_at (facts, 5),
        "amount_candidate", "mail-combined", "42",
        "regex:subject:subject-amount");
}

static void
test_rejects_invalid_regex_rules (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "Project Alpha USD 42",
    };
    const WyreboxDeterministicFactRegexRule invalid_rules[] = {
        {
            .field = "subject",
            .rule_id = "",
            .predicate = "amount_candidate",
            .pattern = "USD[ ]+([0-9]+)",
            .capture_group = 1,
        },
        {
            .field = "date",
            .rule_id = "unsupported-field",
            .predicate = "date_candidate",
            .pattern = "[0-9]{4}-[0-9]{2}-[0-9]{2}",
            .capture_group = 0,
        },
        {
            .field = "subject",
            .rule_id = "unsupported-predicate",
            .predicate = "project_keyword",
            .pattern = "Alpha",
            .capture_group = 0,
        },
        {
            .field = "subject",
            .rule_id = "empty-pattern",
            .predicate = "reference_candidate",
            .pattern = "",
            .capture_group = 0,
        },
        {
            .field = "subject",
            .rule_id = "invalid-pattern",
            .predicate = "reference_candidate",
            .pattern = "(",
            .capture_group = 0,
        },
        {
            .field = "subject",
            .rule_id = "bad-capture",
            .predicate = "amount_candidate",
            .pattern = "USD[ ]+([0-9]+)",
            .capture_group = 2,
        },
    };

    for (guint i = 0; i < G_N_ELEMENTS (invalid_rules); i++) {
        g_autoptr (GError) error = NULL;
        g_autoptr (GPtrArray) facts = NULL;

        facts =
            wyrebox_deterministic_fact_extract_from_metadata_with_regex
                ("mail-invalid-regex", &metadata, 1800000000000000,
                &invalid_rules[i],
                1, &error);
        g_assert_null (facts);
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    }
}

static void
test_rejects_null_regex_rules_with_nonzero_count (void)
{
    WyreboxEmlMetadata metadata = {
        .subject = "Project Alpha USD 42",
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) facts = NULL;

    facts =
        wyrebox_deterministic_fact_extract_from_metadata_with_regex
            ("mail-null-regex", &metadata, 1800000000000000, NULL, 1, &error);
    g_assert_null (facts);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
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

    g_test_add_func ("/facts/deterministic-extractor/"
        "decoded-normalized-header-facts",
        test_extracts_decoded_and_normalized_header_facts);
    g_test_add_func ("/facts/deterministic-extractor/"
        "undecodable-word-raw-fallback",
        test_undecodable_encoded_word_falls_back_to_raw_value);
    g_test_add_func ("/facts/deterministic-extractor/"
        "delivered-to-original-to-fallback",
        test_delivered_to_falls_back_to_x_original_to);
    g_test_add_func ("/facts/deterministic-extractor/"
        "sender-domain-first-valid-from-address",
        test_sender_domain_uses_first_valid_from_address);
    g_test_add_func ("/facts/deterministic-extractor/"
        "unicode-domain-lowercase",
        test_unicode_domains_are_lowercased);
    g_test_add_func ("/facts/deterministic-extractor/"
        "unterminated-comment-rejects-address",
        test_unterminated_comment_rejects_address_member);
    g_test_add_func ("/facts/deterministic-extractor/"
        "invalid-utf8-local-part-is-ignored",
        test_invalid_utf8_local_part_is_ignored);
    g_test_add_func ("/facts/deterministic-extractor/"
        "group-markers-in-quotes-and-comments",
        test_groups_with_quoted_or_commented_markers);
    g_test_add_func ("/facts/deterministic-extractor/header-facts",
        test_extracts_header_facts_from_metadata);
    g_test_add_func ("/facts/deterministic-extractor/only-present-header-facts",
        test_extracts_only_present_header_facts);
    g_test_add_func ("/facts/deterministic-extractor/message-reference-facts",
        test_extracts_message_reference_facts);
    g_test_add_func ("/facts/deterministic-extractor/"
        "ignores-reference-headers-without-message-id-tokens",
        test_ignores_reference_headers_without_message_id_tokens);
    g_test_add_func ("/facts/deterministic-extractor/"
        "dictionary-project-keywords-after-header-facts",
        test_appends_dictionary_project_keywords_after_header_facts);
    g_test_add_func ("/facts/deterministic-extractor/"
        "dictionary-rules-match-only-selected-field",
        test_dictionary_rules_match_only_selected_field);
    g_test_add_func ("/facts/deterministic-extractor/"
        "rejects-invalid-dictionary-rules",
        test_rejects_invalid_dictionary_rules);
    g_test_add_func ("/facts/deterministic-extractor/"
        "regex-subject-amount-capture",
        test_extracts_subject_amount_candidate_from_capture_group);
    g_test_add_func ("/facts/deterministic-extractor/"
        "regex-subject-reference-full-match",
        test_extracts_subject_reference_candidate_from_full_match);
    g_test_add_func ("/facts/deterministic-extractor/"
        "regex-no-match-and-empty-capture",
        test_no_match_and_empty_capture_emit_no_regex_facts);
    g_test_add_func ("/facts/deterministic-extractor/"
        "regex-rule-order-then-match-order",
        test_regex_rules_emit_in_rule_order_then_match_order);
    g_test_add_func ("/facts/deterministic-extractor/"
        "regex-after-header-and-dictionary",
        test_regex_facts_append_after_header_and_dictionary_facts);
    g_test_add_func ("/facts/deterministic-extractor/"
        "rejects-invalid-regex-rules", test_rejects_invalid_regex_rules);
    g_test_add_func ("/facts/deterministic-extractor/"
        "rejects-null-regex-rules-with-nonzero-count",
        test_rejects_null_regex_rules_with_nonzero_count);
    g_test_add_func ("/facts/deterministic-extractor/rejects-missing-mail-id",
        test_rejects_missing_mail_id);
    g_test_add_func (
        "/facts/deterministic-extractor/rejects-missing-created-at",
        test_rejects_missing_created_timestamp);

    return g_test_run ();
}
