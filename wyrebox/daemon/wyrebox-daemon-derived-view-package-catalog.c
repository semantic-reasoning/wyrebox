#include "wyrebox-daemon-derived-view-package-catalog.h"

#include <gio/gio.h>

static const WyreboxDaemonDerivedViewPackageDescriptor catalog[] = {
  {
        "project.view.package",
        "1.0.0",
        "Project view package",
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
