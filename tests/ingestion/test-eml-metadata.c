#include "wyrebox-eml-metadata.h"

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

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

static void
test_parses_simple_crlf_fixture (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_auto (WyreboxEmlMetadata) metadata = { 0 };
  gsize size = 0;

  g_assert_nonnull (fixture_dir);

  bytes = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  size = g_bytes_get_size (bytes);

  g_assert_true (wyrebox_eml_metadata_parse_bytes (bytes, &metadata, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (metadata.message_id, ==, "<simple-crlf@example.test>");
  g_assert_cmpstr (metadata.subject, ==, "CRLF fixture");
  g_assert_cmpstr (metadata.from, ==, "Alice <alice@example.test>");
  g_assert_cmpstr (metadata.to, ==, "Bob <bob@example.test>");
  g_assert_null (metadata.cc);
  g_assert_null (metadata.bcc);
  g_assert_cmpstr (metadata.date, ==, "Tue, 02 Jun 2026 12:34:56 +0000");
  g_assert_cmpuint (metadata.size_bytes, ==, size);
  g_assert_cmpuint (metadata.duplicate_message_id_count, ==, 0);
}

static void
test_missing_message_id_is_successful (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_auto (WyreboxEmlMetadata) metadata = { 0 };

  g_assert_nonnull (fixture_dir);

  bytes = load_fixture_bytes (fixture_dir, "missing-message-id.eml");

  g_assert_true (wyrebox_eml_metadata_parse_bytes (bytes, &metadata, &error));
  g_assert_no_error (error);
  g_assert_null (metadata.message_id);
  g_assert_cmpstr (metadata.subject, ==, "Missing Message-ID fixture");
  g_assert_cmpuint (metadata.duplicate_message_id_count, ==, 0);
}

static void
test_duplicate_message_id_keeps_first_and_counts_extra (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_auto (WyreboxEmlMetadata) metadata = { 0 };

  g_assert_nonnull (fixture_dir);

  bytes = load_fixture_bytes (fixture_dir, "duplicate-message-id.eml");

  g_assert_true (wyrebox_eml_metadata_parse_bytes (bytes, &metadata, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (metadata.message_id, ==,
      "<duplicate-message-id@example.test>");
  g_assert_cmpuint (metadata.duplicate_message_id_count, ==, 1);
}

static void
test_non_ascii_headers_preserve_rfc2047_values (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_auto (WyreboxEmlMetadata) metadata = { 0 };

  g_assert_nonnull (fixture_dir);

  bytes = load_fixture_bytes (fixture_dir, "non-ascii-headers.eml");

  g_assert_true (wyrebox_eml_metadata_parse_bytes (bytes, &metadata, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (metadata.message_id, ==, "<non-ascii-headers@example.test>");
  g_assert_cmpstr (metadata.subject, ==,
      "=?UTF-8?B?UsOpc3Vtw6kg4oCTIOyVhOuFle2VmOyEuOyalA==?=");
  g_assert_cmpstr (metadata.from, ==,
      "=?UTF-8?B?SmnFmcOtIMWgYWZhw61r?= <jiri@example.test>");
  g_assert_cmpstr (metadata.to, ==,
      "=?UTF-8?Q?Zo=C3=AB_Reader?= <zoe@example.test>");
}

static void
test_unfolds_header_continuations (void)
{
  static const char raw[] =
      "From: Folded <folded@example.test>\r\n"
      "To: Target <target@example.test>\r\n"
      "Cc: First <first@example.test>,\r\n"
      "\tSecond <second@example.test>\r\n"
      "Bcc: Hidden <hidden@example.test>\r\n"
      "Subject: First line\r\n"
      " second line\r\n"
      "Date: Thu, 11 Jun 2026 00:00:00 +0000\r\n"
      "Message-ID: <folded@example.test>\r\n" "\r\n" "Body.\r\n";
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_auto (WyreboxEmlMetadata) metadata = { 0 };

  bytes = g_bytes_new_static (raw, strlen (raw));

  g_assert_true (wyrebox_eml_metadata_parse_bytes (bytes, &metadata, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (metadata.subject, ==, "First line second line");
  g_assert_cmpstr (metadata.cc, ==,
      "First <first@example.test>, Second <second@example.test>");
  g_assert_cmpstr (metadata.bcc, ==, "Hidden <hidden@example.test>");
}

static void
test_missing_header_body_separator_is_invalid (void)
{
  static const char raw[] =
      "Subject: No separator\r\nMessage-ID: <bad@test>\r\n";
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_auto (WyreboxEmlMetadata) metadata = { 0 };

  bytes = g_bytes_new_static (raw, strlen (raw));

  g_assert_false (wyrebox_eml_metadata_parse_bytes (bytes, &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (metadata.message_id);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/eml-metadata/simple-crlf",
      test_parses_simple_crlf_fixture);
  g_test_add_func ("/ingestion/eml-metadata/missing-message-id",
      test_missing_message_id_is_successful);
  g_test_add_func ("/ingestion/eml-metadata/duplicate-message-id",
      test_duplicate_message_id_keeps_first_and_counts_extra);
  g_test_add_func ("/ingestion/eml-metadata/non-ascii-headers",
      test_non_ascii_headers_preserve_rfc2047_values);
  g_test_add_func ("/ingestion/eml-metadata/header-continuations",
      test_unfolds_header_continuations);
  g_test_add_func ("/ingestion/eml-metadata/missing-separator",
      test_missing_header_body_separator_is_invalid);

  return g_test_run ();
}
