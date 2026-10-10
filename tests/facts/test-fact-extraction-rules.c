#include "wyrebox-fact-extraction-rules.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

static gchar *
write_rules_file (const gchar *contents)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *dir = NULL;
    gchar *path = NULL;

    dir = g_dir_make_tmp ("wyrebox-fact-extraction-rules-XXXXXX", &error);
    g_assert_no_error (error);
    path = g_build_filename (dir, "extraction.rules", NULL);
    g_assert_true (g_file_set_contents (path, contents, -1, &error));
    g_assert_no_error (error);

    return path;
}

static void
remove_rules_file (const gchar *path)
{
    g_autofree gchar *dir = g_path_get_dirname (path);

    g_assert_cmpint (g_remove (path), ==, 0);
    g_assert_cmpint (g_rmdir (dir), ==, 0);
}

static gboolean
has_fact (GPtrArray *facts, const gchar *predicate, const gchar *value,
    const gchar *source)
{
    for (guint i = 0; i < facts->len; i++) {
        const WyreboxFactRecord *fact = g_ptr_array_index (facts, i);

        if (g_strcmp0 (fact->predicate, predicate) == 0 &&
            g_strcmp0 (fact->args[0], "journal:1:1") == 0 &&
            g_strcmp0 (fact->args[1], value) == 0 &&
            g_strcmp0 (fact->source, source) == 0)
            return TRUE;
    }

    return FALSE;
}

static void
init_metadata (WyreboxEmlMetadata *metadata)
{
    metadata->message_id = g_strdup ("<root@example.test>");
    metadata->subject = g_strdup ("Apollo launch invoice INV-42");
    metadata->from = g_strdup ("Alice <alice@example.test>");
    metadata->in_reply_to = g_strdup ("<parent@example.test>");
}

static void
test_rules_file_drives_extraction (void)
{
    g_autofree gchar *path = write_rules_file (
        "# project dictionary\n"
        "[dictionary:apollo]\n"
        "field=subject\n"
        "match=apollo\n"
        "project=apollo\n"
        "\n"
        "[dictionary:hermes]\n"
        "field=subject\n"
        "match=hermes\n"
        "project=hermes\n"
        "\n"
        "[regex:invoice]\n"
        "field=subject\n"
        "predicate=reference_candidate\n"
        "pattern=INV-(\\d+)\n"
        "capture_group=1\n");
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxFactExtractionRules) rules = NULL;
    g_autoptr (GPtrArray) facts = NULL;
    g_auto (WyreboxEmlMetadata) metadata = { 0 };

    rules = wyrebox_fact_extraction_rules_new_from_file (path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (rules);

    init_metadata (&metadata);
    facts = wyrebox_fact_extraction_rules_extract (rules, "journal:1:1",
            &metadata, 7, &error);
    g_assert_no_error (error);
    g_assert_true (has_fact (facts, "message_id", "<root@example.test>",
        "header:message-id"));
    g_assert_true (has_fact (facts, "replies_to", "<parent@example.test>",
        "header:in-reply-to"));
    g_assert_true (has_fact (facts, "project_keyword", "apollo",
        "dictionary:subject:apollo"));
    g_assert_false (has_fact (facts, "project_keyword", "hermes",
        "dictionary:subject:hermes"));
    g_assert_true (has_fact (facts, "reference_candidate", "42",
        "regex:subject:invoice"));

    remove_rules_file (path);
}

static void
test_empty_rules_extract_header_facts (void)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxFactExtractionRules) rules =
        wyrebox_fact_extraction_rules_new_empty ();
    g_autoptr (GPtrArray) facts = NULL;
    g_auto (WyreboxEmlMetadata) metadata = { 0 };

    init_metadata (&metadata);
    facts = wyrebox_fact_extraction_rules_extract (rules, "journal:1:1",
            &metadata, 7, &error);
    g_assert_no_error (error);
    g_assert_true (has_fact (facts, "sender_domain", "example.test",
        "header:from"));
    for (guint i = 0; i < facts->len; i++) {
        const WyreboxFactRecord *fact = g_ptr_array_index (facts, i);

        g_assert_true (g_str_has_prefix (fact->source, "header:"));
    }
}

static void
assert_rules_rejected (const gchar *contents, const gchar *message)
{
    g_autofree gchar *path = write_rules_file (contents);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxFactExtractionRules) rules = NULL;

    rules = wyrebox_fact_extraction_rules_new_from_file (path, &error);
    g_assert_null (rules);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    if (g_strstr_len (error->message, -1, message) == NULL)
        g_error ("expected '%s' in '%s'", message, error->message);

    remove_rules_file (path);
}

static void
test_invalid_rules_are_rejected (void)
{
    assert_rules_rejected ("[project:apollo]\nfield=subject\n",
        "unsupported group 'project:apollo'");
    assert_rules_rejected ("[dictionary:apollo/x]\nfield=subject\n"
        "match=a\nproject=a\n", "invalid rule id 'apollo/x'");
    assert_rules_rejected ("[dictionary:apollo]\nfield=subject\n"
        "match=a\n", "dictionary:apollo is missing project");
    assert_rules_rejected ("[dictionary:apollo]\nfield=subject\n"
        "match=a\nproject=a\nweight=2\n",
        "dictionary:apollo has unknown key 'weight'");
    assert_rules_rejected ("[dictionary:apollo]\nfield=body\n"
        "match=a\nproject=a\n", "unsupported dictionary rule field 'body'");
    assert_rules_rejected ("[regex:invoice]\nfield=subject\n"
        "predicate=project_keyword\npattern=INV\n",
        "unsupported regex rule predicate 'project_keyword'");
    assert_rules_rejected ("[regex:invoice]\nfield=subject\n"
        "predicate=reference_candidate\npattern=INV-(\n",
        "invalid regex pattern for rule 'invoice'");
    assert_rules_rejected ("[regex:invoice]\nfield=subject\n"
        "predicate=reference_candidate\npattern=INV\ncapture_group=1\n",
        "capture group 1 is out of range");
    assert_rules_rejected ("[regex:invoice]\nfield=subject\n"
        "predicate=reference_candidate\npattern=INV\ncapture_group=one\n",
        "regex:invoice capture_group must be a non-negative integer");
    assert_rules_rejected ("field=subject\n", "cannot be parsed");
}

static void
test_missing_rules_file_is_rejected (void)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxFactExtractionRules) rules = NULL;

    rules = wyrebox_fact_extraction_rules_new_from_file (
        "/nonexistent/wyrebox/extraction.rules", &error);
    g_assert_null (rules);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (g_strstr_len (error->message, -1, "cannot be read"));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/facts/extraction-rules/file-drives-extraction",
        test_rules_file_drives_extraction);
    g_test_add_func ("/facts/extraction-rules/empty-extracts-headers",
        test_empty_rules_extract_header_facts);
    g_test_add_func ("/facts/extraction-rules/rejects-invalid",
        test_invalid_rules_are_rejected);
    g_test_add_func ("/facts/extraction-rules/rejects-missing-file",
        test_missing_rules_file_is_rejected);

    return g_test_run ();
}
