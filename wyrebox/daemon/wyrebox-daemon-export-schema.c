#include "wyrebox-daemon-export-schema.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "export schema metadata %s is required", field_name);
    return FALSE;
  }

  return TRUE;
}

void
wyrebox_daemon_export_schema_metadata_clear (WyreboxDaemonExportSchemaMetadata
    *metadata)
{
  if (metadata == NULL)
    return;

  g_clear_pointer (&metadata->schema_id, g_free);
  g_clear_pointer (&metadata->schema_version, g_free);
  g_clear_pointer (&metadata->dataset_id, g_free);
  g_clear_pointer (&metadata->content_format, g_free);
  g_clear_pointer (&metadata->ordering_contract, g_free);
  g_clear_pointer (&metadata->object_reference_contract, g_free);
}

const char *
wyrebox_daemon_export_order_to_string (WyreboxDaemonExportOrder order)
{
  switch (order) {
    case WYREBOX_DAEMON_EXPORT_ORDER_BY_JOURNAL_POSITION:
      return "journal-position";
    case WYREBOX_DAEMON_EXPORT_ORDER_BY_TIME_RANGE:
      return "time-range";
    default:
      return NULL;
  }
}

gboolean
wyrebox_daemon_export_schema_metadata_init (WyreboxDaemonExportSchemaMetadata
    *metadata, const char *schema_id, const char *schema_version,
    const char *dataset_id, const char *content_format,
    const char *ordering_contract, const char *object_reference_contract,
    GError **error)
{
  g_auto (WyreboxDaemonExportSchemaMetadata) next = { 0 };

  g_return_val_if_fail (metadata != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_text (schema_id, "schema_id", error) ||
      !validate_required_text (schema_version, "schema_version", error) ||
      !validate_required_text (dataset_id, "dataset_id", error) ||
      !validate_required_text (content_format, "content_format", error) ||
      !validate_required_text (ordering_contract, "ordering_contract",
          error) ||
      !validate_required_text (object_reference_contract,
          "object_reference_contract", error))
    return FALSE;

  next.schema_id = g_strdup (schema_id);
  next.schema_version = g_strdup (schema_version);
  next.dataset_id = g_strdup (dataset_id);
  next.content_format = g_strdup (content_format);
  next.ordering_contract = g_strdup (ordering_contract);
  next.object_reference_contract = g_strdup (object_reference_contract);

  wyrebox_daemon_export_schema_metadata_clear (metadata);
  *metadata = next;
  next.schema_id = NULL;
  next.schema_version = NULL;
  next.dataset_id = NULL;
  next.content_format = NULL;
  next.ordering_contract = NULL;
  next.object_reference_contract = NULL;

  return TRUE;
}

gboolean
wyrebox_daemon_export_object_reference_contract_is_stable (const char
    *object_reference_contract, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_text (object_reference_contract,
          "object_reference_contract", error))
    return FALSE;

  if (g_strcmp0 (object_reference_contract, "sha256+object-key") != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "export object reference contract must be sha256+object-key");
    return FALSE;
  }

  return TRUE;
}
