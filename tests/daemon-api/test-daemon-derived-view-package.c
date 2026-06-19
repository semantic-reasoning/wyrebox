#include "wyrebox-daemon-derived-view-package.h"
#include "wyrebox-daemon-derived-view-package-catalog.h"

#include <gio/gio.h>

static WyreboxDaemonDerivedViewPackageManifest
make_valid_manifest (void)
{
  WyreboxDaemonDerivedViewPackageManifest manifest = { 0 };

  manifest.package_name = g_strdup ("project.view.package");
  manifest.package_version = g_strdup ("1.0.0");
  manifest.description = g_strdup ("Project view package");
  manifest.declared_inputs = g_new0 (gchar *, 3);
  manifest.declared_inputs[0] = g_strdup ("mail.message");
  manifest.declared_inputs[1] = g_strdup ("mail.fact");
  manifest.declared_outputs = g_new0 (gchar *, 2);
  manifest.declared_outputs[0] = g_strdup ("mail.view");
  manifest.compatible_schema_version = g_strdup ("schema-7");
  manifest.compatible_api_version = g_strdup ("api-3");
  manifest.rules_source = g_strdup ("view('project').");
  manifest.author = g_strdup ("wyrebox");
  manifest.source_ref = g_strdup ("git+https://example.invalid/package");

  return manifest;
}

static void
assert_invalid (WyreboxDaemonDerivedViewPackageManifest *manifest,
    const gchar *expected_message)
{
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_derived_view_package_manifest_validate
      (manifest, &error));
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==, expected_message);
}

static void
test_valid_manifest (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonDerivedViewPackageManifest) manifest =
      make_valid_manifest ();

  g_assert_true (wyrebox_daemon_derived_view_package_manifest_validate
      (&manifest, &error));
  g_assert_no_error (error);
}

static void
test_rejects_missing_required_field (void)
{
  WyreboxDaemonDerivedViewPackageManifest manifest = make_valid_manifest ();

  g_clear_pointer (&manifest.package_version, g_free);
  assert_invalid (&manifest,
      "derived view package field 'package_version' is required");
  wyrebox_daemon_derived_view_package_manifest_clear (&manifest);
}

static void
test_rejects_duplicate_declared_inputs (void)
{
  WyreboxDaemonDerivedViewPackageManifest manifest = make_valid_manifest ();

  g_free (manifest.declared_inputs[1]);
  manifest.declared_inputs[1] = g_strdup ("mail.message");

  assert_invalid (&manifest,
      "derived view package field 'declared_inputs' contains duplicate entry "
      "'mail.message'");
  wyrebox_daemon_derived_view_package_manifest_clear (&manifest);
}

static void
test_rejects_control_characters (void)
{
  WyreboxDaemonDerivedViewPackageManifest manifest = make_valid_manifest ();

  g_free (manifest.description);
  manifest.description = g_strdup ("Project\nview package");

  assert_invalid (&manifest,
      "derived view package field 'description' must not contain control "
      "characters");
  wyrebox_daemon_derived_view_package_manifest_clear (&manifest);
}

static void
test_matches_catalog_descriptor (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonDerivedViewPackageManifest) manifest =
      make_valid_manifest ();
  const WyreboxDaemonDerivedViewPackageDescriptor *descriptor = NULL;

  descriptor = wyrebox_daemon_derived_view_package_catalog_lookup
      (manifest.package_name, manifest.package_version);
  g_assert_nonnull (descriptor);
  g_assert_true (wyrebox_daemon_derived_view_package_manifest_matches_descriptor
      (&manifest, descriptor, &error));
  g_assert_no_error (error);
}

static void
test_rejects_mismatched_catalog_descriptor (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonDerivedViewPackageManifest) manifest =
      make_valid_manifest ();
  WyreboxDaemonDerivedViewPackageDescriptor descriptor = {
    .package_name = "project.view.package",
    .package_version = "2.0.0",
    .description = "Project view package",
    .compatible_schema_version = "schema-7",
    .compatible_api_version = "api-3",
    .rules_source = "view('project').",
    .author = "wyrebox",
    .source_ref = "git+https://example.invalid/package",
  };

  g_assert_false
      (wyrebox_daemon_derived_view_package_manifest_matches_descriptor
      (&manifest, &descriptor, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon/derived-view-package/valid", test_valid_manifest);
  g_test_add_func ("/daemon/derived-view-package/missing-required-field",
      test_rejects_missing_required_field);
  g_test_add_func ("/daemon/derived-view-package/duplicate-declared-inputs",
      test_rejects_duplicate_declared_inputs);
  g_test_add_func ("/daemon/derived-view-package/control-characters",
      test_rejects_control_characters);
  g_test_add_func ("/daemon/derived-view-package/matches-catalog-descriptor",
      test_matches_catalog_descriptor);
  g_test_add_func
      ("/daemon/derived-view-package/rejects-mismatched-catalog-descriptor",
      test_rejects_mismatched_catalog_descriptor);

  return g_test_run ();
}
