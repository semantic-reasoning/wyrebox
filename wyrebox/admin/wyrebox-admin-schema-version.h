#pragma once

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  gboolean json;
} WyreboxAdminSchemaVersionOptions;

typedef struct
{
  guint64 schema_version;
} WyreboxAdminSchemaVersionResult;

void wyrebox_admin_schema_version_options_clear (
    WyreboxAdminSchemaVersionOptions *options);
void wyrebox_admin_schema_version_result_clear (
    WyreboxAdminSchemaVersionResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxAdminSchemaVersionOptions,
    wyrebox_admin_schema_version_options_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxAdminSchemaVersionResult,
    wyrebox_admin_schema_version_result_clear)

int wyrebox_admin_schema_version_probe (
    WyreboxAdminSchemaVersionResult *result);

G_END_DECLS
/* *INDENT-ON* */
