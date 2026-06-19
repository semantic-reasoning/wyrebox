#include "wyrebox-daemon-derived-view-package.h"

#include <gio/gio.h>

static gboolean
is_nonempty_text (const gchar *value)
{
  return value != NULL && value[0] != '\0';
}

static gboolean
contains_control_character (const gchar *value)
{
  for (const gchar * cursor = value; cursor != NULL && *cursor != '\0';
      cursor++) {
    if (g_ascii_iscntrl (*cursor))
      return TRUE;
  }

  return FALSE;
}

static gboolean
validate_text_field (const gchar *value, const gchar *field_name,
    gboolean allow_empty, GError **error)
{
  if (!allow_empty && !is_nonempty_text (value)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package field '%s' is required", field_name);
    return FALSE;
  }

  if (value != NULL && contains_control_character (value)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package field '%s' must not contain control "
        "characters", field_name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_string_vector (gchar **values, const gchar *field_name, GError **error)
{
  g_autoptr (GHashTable) seen = NULL;

  if (values == NULL || values[0] == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package field '%s' requires at least one entry",
        field_name);
    return FALSE;
  }

  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; values[i] != NULL; i++) {
    const gchar *value = values[i];

    if (!is_nonempty_text (value)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "derived view package field '%s' contains an empty entry",
          field_name);
      return FALSE;
    }

    if (contains_control_character (value)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "derived view package field '%s' contains control characters",
          field_name);
      return FALSE;
    }

    if (g_hash_table_contains (seen, value)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "derived view package field '%s' contains duplicate entry '%s'",
          field_name, value);
      return FALSE;
    }

    g_hash_table_add (seen, g_strdup (value));
  }

  return TRUE;
}

static gboolean
compare_string_vector (gchar **values, const char *const *expected_values,
    const gchar *field_name, GError **error)
{
  guint i = 0;

  for (i = 0; values != NULL && values[i] != NULL &&
      expected_values != NULL && expected_values[i] != NULL; i++) {
    if (g_strcmp0 (values[i], expected_values[i]) != 0) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "derived view package manifest field '%s' does not match catalog "
          "descriptor", field_name);
      return FALSE;
    }
  }

  if ((values != NULL && values[i] != NULL) ||
      (expected_values != NULL && expected_values[i] != NULL)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package manifest field '%s' does not match catalog "
        "descriptor", field_name);
    return FALSE;
  }

  return TRUE;
}

void wyrebox_daemon_derived_view_package_manifest_clear
    (WyreboxDaemonDerivedViewPackageManifest * manifest)
{
  if (manifest == NULL)
    return;

  g_clear_pointer (&manifest->package_name, g_free);
  g_clear_pointer (&manifest->package_version, g_free);
  g_clear_pointer (&manifest->description, g_free);
  g_strfreev (manifest->declared_inputs);
  manifest->declared_inputs = NULL;
  g_strfreev (manifest->declared_outputs);
  manifest->declared_outputs = NULL;
  g_clear_pointer (&manifest->compatible_schema_version, g_free);
  g_clear_pointer (&manifest->compatible_api_version, g_free);
  g_clear_pointer (&manifest->rules_source, g_free);
  g_clear_pointer (&manifest->author, g_free);
  g_clear_pointer (&manifest->source_ref, g_free);
}

static void
package_manifest_free (gpointer data)
{
  WyreboxDaemonDerivedViewPackageManifest *manifest = data;

  if (manifest == NULL)
    return;

  wyrebox_daemon_derived_view_package_manifest_clear (manifest);
  g_free (manifest);
}

void wyrebox_daemon_derived_view_package_manifest_free
    (WyreboxDaemonDerivedViewPackageManifest * manifest)
{
  package_manifest_free (manifest);
}

gboolean
wyrebox_daemon_derived_view_package_manifest_validate (const
    WyreboxDaemonDerivedViewPackageManifest *manifest, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (manifest == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package manifest is required");
    return FALSE;
  }

  if (!validate_text_field (manifest->package_name, "package_name", FALSE,
          error) ||
      !validate_text_field (manifest->package_version, "package_version",
          FALSE, error) ||
      !validate_text_field (manifest->description, "description", FALSE,
          error) ||
      !validate_string_vector (manifest->declared_inputs, "declared_inputs",
          error) ||
      !validate_string_vector (manifest->declared_outputs, "declared_outputs",
          error) ||
      !validate_text_field (manifest->compatible_schema_version,
          "compatible_schema_version", FALSE, error) ||
      !validate_text_field (manifest->compatible_api_version,
          "compatible_api_version", FALSE, error) ||
      !validate_text_field (manifest->rules_source, "rules_source", FALSE,
          error))
    return FALSE;

  if (manifest->author != NULL &&
      !validate_text_field (manifest->author, "author", TRUE, error))
    return FALSE;

  if (manifest->source_ref != NULL &&
      !validate_text_field (manifest->source_ref, "source_ref", TRUE, error))
    return FALSE;

  return TRUE;
}

gboolean
wyrebox_daemon_derived_view_package_manifest_matches_descriptor (const
    WyreboxDaemonDerivedViewPackageManifest *manifest,
    const WyreboxDaemonDerivedViewPackageDescriptor *descriptor, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_daemon_derived_view_package_manifest_validate (manifest, error))
    return FALSE;

  if (descriptor == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package descriptor is required");
    return FALSE;
  }

  if (g_strcmp0 (manifest->package_name, descriptor->package_name) != 0 ||
      g_strcmp0 (manifest->package_version, descriptor->package_version) != 0 ||
      g_strcmp0 (manifest->description, descriptor->description) != 0 ||
      g_strcmp0 (manifest->compatible_schema_version,
          descriptor->compatible_schema_version) != 0 ||
      g_strcmp0 (manifest->compatible_api_version,
          descriptor->compatible_api_version) != 0 ||
      g_strcmp0 (manifest->rules_source, descriptor->rules_source) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package manifest does not match catalog descriptor");
    return FALSE;
  }

  if (!compare_string_vector (manifest->declared_inputs,
          descriptor->declared_inputs, "declared_inputs", error) ||
      !compare_string_vector (manifest->declared_outputs,
          descriptor->declared_outputs, "declared_outputs", error))
    return FALSE;

  if (g_strcmp0 (manifest->author, descriptor->author) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package manifest does not match catalog author");
    return FALSE;
  }

  if (g_strcmp0 (manifest->source_ref, descriptor->source_ref) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view package manifest does not match catalog source");
    return FALSE;
  }

  return TRUE;
}
