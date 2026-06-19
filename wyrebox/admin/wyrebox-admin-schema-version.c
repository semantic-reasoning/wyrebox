#include "wyrebox-admin-schema-version.h"

#include "wyrebox-schema-migration.h"

void
wyrebox_admin_schema_version_options_clear (WyreboxAdminSchemaVersionOptions
    *options)
{
  if (options == NULL)
    return;

  options->json = FALSE;
}

void
wyrebox_admin_schema_version_result_clear (WyreboxAdminSchemaVersionResult
    *result)
{
  if (result == NULL)
    return;

  result->schema_version = 0;
}

int
wyrebox_admin_schema_version_probe (WyreboxAdminSchemaVersionResult *result)
{
  g_return_val_if_fail (result != NULL, G_IO_ERROR_INVALID_ARGUMENT);

  wyrebox_admin_schema_version_result_clear (result);
  result->schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  return 0;
}
