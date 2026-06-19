#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  const char *package_name;
  const char *package_version;
  const char *description;
  const char *compatible_schema_version;
  const char *compatible_api_version;
  const char *rules_source;
  const char *author;
  const char *source_ref;
} WyreboxDaemonDerivedViewPackageDescriptor;

const WyreboxDaemonDerivedViewPackageDescriptor *
wyrebox_daemon_derived_view_package_catalog_lookup (const char *package_name,
    const char *package_version);

gsize wyrebox_daemon_derived_view_package_catalog_size (void);

const WyreboxDaemonDerivedViewPackageDescriptor *
wyrebox_daemon_derived_view_package_catalog_at (gsize index);

G_END_DECLS
/* *INDENT-ON* */
