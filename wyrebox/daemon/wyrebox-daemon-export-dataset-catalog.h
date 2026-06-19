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
  const char *description;
} WyreboxDaemonExportDatasetDescriptor;

const WyreboxDaemonExportDatasetDescriptor *
wyrebox_daemon_export_dataset_catalog_lookup (const char *dataset_id);

G_END_DECLS
/* *INDENT-ON* */
