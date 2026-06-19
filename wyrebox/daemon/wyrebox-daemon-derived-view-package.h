#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  gchar *package_name;
  gchar *package_version;
  gchar *description;
  gchar **declared_inputs;
  gchar **declared_outputs;
  gchar *compatible_schema_version;
  gchar *compatible_api_version;
  gchar *rules_source;
  gchar *author;
  gchar *source_ref;
} WyreboxDaemonDerivedViewPackageManifest;

void wyrebox_daemon_derived_view_package_manifest_clear (
    WyreboxDaemonDerivedViewPackageManifest *manifest);

void wyrebox_daemon_derived_view_package_manifest_free (
    WyreboxDaemonDerivedViewPackageManifest *manifest);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonDerivedViewPackageManifest,
    wyrebox_daemon_derived_view_package_manifest_clear)

gboolean wyrebox_daemon_derived_view_package_manifest_validate (
    const WyreboxDaemonDerivedViewPackageManifest *manifest,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
