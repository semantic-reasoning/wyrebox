#include "wyrebox-daemon-derived-view-package-catalog.h"

#include <gio/gio.h>

static void
test_catalog_resolves_project_package (void)
{
  const WyreboxDaemonDerivedViewPackageDescriptor *descriptor = NULL;

  descriptor = wyrebox_daemon_derived_view_package_catalog_lookup
      ("project.view.package", "1.0.0");
  g_assert_nonnull (descriptor);
  g_assert_cmpstr (descriptor->description, ==, "Project view package");
  g_assert_cmpstr (descriptor->compatible_schema_version, ==, "schema-7");
  g_assert_cmpstr (descriptor->compatible_api_version, ==, "api-3");
}

static void
test_catalog_rejects_unknown_package (void)
{
  g_assert_null (wyrebox_daemon_derived_view_package_catalog_lookup
      ("unknown.package", "1.0.0"));
  g_assert_null (wyrebox_daemon_derived_view_package_catalog_lookup
      ("project.view.package", "2.0.0"));
  g_assert_null (wyrebox_daemon_derived_view_package_catalog_lookup (NULL,
          NULL));
}

static void
test_catalog_is_enumerable (void)
{
  static const char *expected_names[] = {
    "project.view.package",
  };
  static const char *expected_versions[] = {
    "1.0.0",
  };

  g_assert_cmpuint (wyrebox_daemon_derived_view_package_catalog_size (), ==,
      G_N_ELEMENTS (expected_names));

  for (gsize index = 0; index < G_N_ELEMENTS (expected_names); index++) {
    const WyreboxDaemonDerivedViewPackageDescriptor *descriptor = NULL;

    descriptor = wyrebox_daemon_derived_view_package_catalog_at (index);
    g_assert_nonnull (descriptor);
    g_assert_cmpstr (descriptor->package_name, ==, expected_names[index]);
    g_assert_cmpstr (descriptor->package_version, ==, expected_versions[index]);
  }

  g_assert_null (wyrebox_daemon_derived_view_package_catalog_at (1));
}

static void
test_catalog_validates_all_entries (void)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_derived_view_package_catalog_validate_all
      (&error));
  g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon-api/derived-view-package-catalog/resolves-project-package",
      test_catalog_resolves_project_package);
  g_test_add_func
      ("/daemon-api/derived-view-package-catalog/rejects-unknown-package",
      test_catalog_rejects_unknown_package);
  g_test_add_func ("/daemon-api/derived-view-package-catalog/is-enumerable",
      test_catalog_is_enumerable);
  g_test_add_func ("/daemon-api/derived-view-package-catalog/validate-all",
      test_catalog_validates_all_entries);

  return g_test_run ();
}
