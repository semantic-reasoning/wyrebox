#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  const char *package_name;
  const char *package_version;
  const char *description;
  const char *const *declared_inputs;
  const char *const *declared_outputs;
  const char *compatible_schema_version;
  const char *compatible_api_version;
  const char *rules_source;
  const char *author;
  const char *source_ref;
} WyreboxDaemonDerivedViewPackageDescriptor;

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

gboolean wyrebox_daemon_derived_view_package_manifest_matches_descriptor (
    const WyreboxDaemonDerivedViewPackageManifest *manifest,
    const WyreboxDaemonDerivedViewPackageDescriptor *descriptor,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
