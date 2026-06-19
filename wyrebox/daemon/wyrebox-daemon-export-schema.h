#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_DAEMON_EXPORT_ORDER_BY_JOURNAL_POSITION = 1,
  WYREBOX_DAEMON_EXPORT_ORDER_BY_TIME_RANGE = 2,
} WyreboxDaemonExportOrder;

typedef struct
{
  char *schema_id;
  char *schema_version;
  char *dataset_id;
  char *content_format;
  char *ordering_contract;
  char *object_reference_contract;
} WyreboxDaemonExportSchemaMetadata;

void wyrebox_daemon_export_schema_metadata_clear (
    WyreboxDaemonExportSchemaMetadata *metadata);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonExportSchemaMetadata,
    wyrebox_daemon_export_schema_metadata_clear)

gboolean wyrebox_daemon_export_schema_metadata_init (
    WyreboxDaemonExportSchemaMetadata *metadata,
    const char *schema_id,
    const char *schema_version,
    const char *dataset_id,
    const char *content_format,
    const char *ordering_contract,
    const char *object_reference_contract,
    GError **error);

const char *wyrebox_daemon_export_order_to_string (
    WyreboxDaemonExportOrder order);

G_END_DECLS
/* *INDENT-ON* */
