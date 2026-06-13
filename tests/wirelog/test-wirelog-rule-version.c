#include "wyrebox-wirelog-rule-version.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

static void
assert_valid_rule_version_hash (const char *hash)
{
  g_assert_nonnull (hash);
  g_assert_cmpuint (strlen (hash), ==, 71);
  g_assert_true (g_str_has_prefix (hash, "sha256:"));

  for (guint i = 7; i < 71; i++)
    g_assert_true (g_ascii_isxdigit (hash[i]) && !g_ascii_isupper (hash[i]));
}

static char *
hash_rules (const char *rules_source)
{
  g_autoptr (GError) error = NULL;
  char *hash = NULL;

  hash = wyrebox_wirelog_rule_version_hash (rules_source, &error);
  g_assert_no_error (error);
  assert_valid_rule_version_hash (hash);

  return hash;
}

static void
test_same_source_gives_same_hash (void)
{
  g_autofree char *first = NULL;
  g_autofree char *second = NULL;

  first = hash_rules ("view(mail) :- project_keyword(mail, \"alpha\").\n");
  second = hash_rules ("view(mail) :- project_keyword(mail, \"alpha\").\n");

  g_assert_cmpstr (first, ==, second);
}

static void
test_different_source_gives_different_hash (void)
{
  g_autofree char *first = NULL;
  g_autofree char *second = NULL;

  first = hash_rules ("view(mail) :- project_keyword(mail, \"alpha\").\n");
  second = hash_rules ("view(mail) :- project_keyword(mail, \"beta\").\n");

  g_assert_cmpstr (first, !=, second);
}

static void
test_byte_for_byte_policy_hashes_formatting_changes (void)
{
  g_autofree char *base = NULL;
  g_autofree char *extra_space = NULL;
  g_autofree char *comment = NULL;
  g_autofree char *crlf = NULL;

  base = hash_rules ("view(mail) :- project_keyword(mail, \"alpha\").\n");
  extra_space =
      hash_rules ("view(mail)  :- project_keyword(mail, \"alpha\").\n");
  comment =
      hash_rules ("# alpha project\n"
      "view(mail) :- project_keyword(mail, \"alpha\").\n");
  crlf = hash_rules ("view(mail) :- project_keyword(mail, \"alpha\").\r\n");

  g_assert_cmpstr (base, !=, extra_space);
  g_assert_cmpstr (base, !=, comment);
  g_assert_cmpstr (base, !=, crlf);
}

static void
test_null_source_fails (void)
{
  g_autofree char *hash = NULL;
  g_autoptr (GError) error = NULL;

  hash = wyrebox_wirelog_rule_version_hash (NULL, &error);

  g_assert_null (hash);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_empty_source_fails (void)
{
  g_autofree char *hash = NULL;
  g_autoptr (GError) error = NULL;

  hash = wyrebox_wirelog_rule_version_hash ("", &error);

  g_assert_null (hash);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/rule-version/same-source",
      test_same_source_gives_same_hash);
  g_test_add_func ("/wirelog/rule-version/different-source",
      test_different_source_gives_different_hash);
  g_test_add_func ("/wirelog/rule-version/byte-for-byte",
      test_byte_for_byte_policy_hashes_formatting_changes);
  g_test_add_func ("/wirelog/rule-version/null-source", test_null_source_fails);
  g_test_add_func ("/wirelog/rule-version/empty-source",
      test_empty_source_fails);

  return g_test_run ();
}
