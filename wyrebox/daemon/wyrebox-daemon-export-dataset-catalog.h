#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  const char *dataset_id;
  const char *name;
  const char *schema_version;
  const char *output_format;
  const char *incremental_cursor;
  const char *stable_ordering;
  const char *authorization_scope;
  const char *description;
} WyreboxDaemonExportDatasetDescriptor;

const WyreboxDaemonExportDatasetDescriptor *
wyrebox_daemon_export_dataset_catalog_lookup (const char *dataset_id);

gsize wyrebox_daemon_export_dataset_catalog_size (void);

const WyreboxDaemonExportDatasetDescriptor *
wyrebox_daemon_export_dataset_catalog_at (gsize index);

G_END_DECLS
/* *INDENT-ON* */
