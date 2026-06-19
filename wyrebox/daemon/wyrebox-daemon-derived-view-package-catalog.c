#include "wyrebox-daemon-derived-view-package-catalog.h"

#include <gio/gio.h>

static const WyreboxDaemonDerivedViewPackageDescriptor catalog[] = {
  {
        "project.view.package",
        "1.0.0",
        "Project view package",
        (const char *const[]) {
              "mail.message",
              "mail.fact",
              NULL,
            },
        (const char *const[]) {
              "mail.view",
              NULL,
            },
        "schema-7",
        "api-3",
        "view('project').",
        "wyrebox",
        "git+https://example.invalid/package",
      },
};

const WyreboxDaemonDerivedViewPackageDescriptor *
wyrebox_daemon_derived_view_package_catalog_lookup (const char *package_name,
    const char *package_version)
{
  if (package_name == NULL || *package_name == '\0' || package_version == NULL
      || *package_version == '\0')
    return NULL;

  for (gsize i = 0; i < G_N_ELEMENTS (catalog); i++) {
    if (g_strcmp0 (catalog[i].package_name, package_name) == 0 &&
        g_strcmp0 (catalog[i].package_version, package_version) == 0)
      return &catalog[i];
  }

  return NULL;
}

gsize
wyrebox_daemon_derived_view_package_catalog_size (void)
{
  return G_N_ELEMENTS (catalog);
}

const WyreboxDaemonDerivedViewPackageDescriptor *
wyrebox_daemon_derived_view_package_catalog_at (gsize index)
{
  if (index >= G_N_ELEMENTS (catalog))
    return NULL;

  return &catalog[index];
}

gboolean
wyrebox_daemon_derived_view_package_catalog_validate_all (GError **error)
{
  for (gsize i = 0; i < G_N_ELEMENTS (catalog); i++) {
    WyreboxDaemonDerivedViewPackageManifest manifest = { 0 };

    manifest.package_name = (gchar *) catalog[i].package_name;
    manifest.package_version = (gchar *) catalog[i].package_version;
    manifest.description = (gchar *) catalog[i].description;
    manifest.declared_inputs = (gchar **) catalog[i].declared_inputs;
    manifest.declared_outputs = (gchar **) catalog[i].declared_outputs;
    manifest.compatible_schema_version =
        (gchar *) catalog[i].compatible_schema_version;
    manifest.compatible_api_version =
        (gchar *) catalog[i].compatible_api_version;
    manifest.rules_source = (gchar *) catalog[i].rules_source;
    manifest.author = (gchar *) catalog[i].author;
    manifest.source_ref = (gchar *) catalog[i].source_ref;

    if (!wyrebox_daemon_derived_view_package_manifest_matches_descriptor
        (&manifest, &catalog[i], error))
      return FALSE;
  }

  return TRUE;
}
