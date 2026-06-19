#pragma once

#include "wyrebox-daemon-derived-view-package.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

const WyreboxDaemonDerivedViewPackageDescriptor *
wyrebox_daemon_derived_view_package_catalog_lookup (const char *package_name,
    const char *package_version);

gsize wyrebox_daemon_derived_view_package_catalog_size (void);

const WyreboxDaemonDerivedViewPackageDescriptor *
wyrebox_daemon_derived_view_package_catalog_at (gsize index);

gboolean wyrebox_daemon_derived_view_package_catalog_validate_all (GError **error);

G_END_DECLS
/* *INDENT-ON* */
