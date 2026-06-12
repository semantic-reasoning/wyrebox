#include "wyrebox-daemon-duckdb-query-template-request.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template %s is required", field_name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_no_control_characters (const char *value, const char *field_name,
    GError **error)
{
  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template %s must not contain control characters",
          field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_conservative_token (const char *value, const char *field_name,
    GError **error)
{
  if (!validate_required_text (value, field_name, error))
    return FALSE;

  if (!validate_no_control_characters (value, field_name, error))
    return FALSE;

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (!(g_ascii_isalnum (*cursor) || *cursor == '_'
            || *cursor == '-' || *cursor == '.' || *cursor == ':')) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template %s contains invalid characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
copy_parameters (const char *const *parameters, gchar ***out_parameters,
    GError **error)
{
  g_auto (GStrv) copied = NULL;
  gsize parameter_count = 0;

  g_return_val_if_fail (out_parameters != NULL, FALSE);

  *out_parameters = NULL;

  if (parameters == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameters is required");
    return FALSE;
  }

  while (parameters[parameter_count] != NULL) {
    const char *parameter = parameters[parameter_count];

    if (parameter == NULL || *parameter == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template parameter is required");
      return FALSE;
    }

    if (!validate_no_control_characters (parameter, "parameter", error))
      return FALSE;

    parameter_count++;
  }

  copied = g_new0 (gchar *, parameter_count + 1);

  for (gsize i = 0; i < parameter_count; i++)
    copied[i] = g_strdup (parameters[i]);

  *out_parameters = g_steal_pointer (&copied);
  return TRUE;
}

void wyrebox_daemon_duckdb_query_template_request_clear
    (WyreboxDaemonDuckDBQueryTemplateRequest * request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->query_id, g_free);
  g_clear_pointer (&request->template_id, g_free);
  g_clear_pointer (&request->scope_id, g_free);
  g_clear_pointer (&request->parameters, g_strfreev);
}

gboolean
    wyrebox_daemon_duckdb_query_template_request_init
    (WyreboxDaemonDuckDBQueryTemplateRequest * request, const char *query_id,
    const char *template_id, const char *scope_id,
    const char *const *parameters, GError ** error)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) next = { 0 };
  g_auto (GStrv) next_parameters = NULL;

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_conservative_token (query_id, "query_id", error))
    return FALSE;

  if (!validate_conservative_token (template_id, "template_id", error))
    return FALSE;

  if (!validate_required_text (scope_id, "scope_id", error))
    return FALSE;

  if (!validate_no_control_characters (scope_id, "scope_id", error))
    return FALSE;

  if (!copy_parameters (parameters, &next_parameters, error))
    return FALSE;

  next.query_id = g_strdup (query_id);
  next.template_id = g_strdup (template_id);
  next.scope_id = g_strdup (scope_id);
  next.parameters = g_steal_pointer (&next_parameters);

  wyrebox_daemon_duckdb_query_template_request_clear (request);
  *request = next;
  next.query_id = NULL;
  next.template_id = NULL;
  next.scope_id = NULL;
  next.parameters = NULL;

  return TRUE;
}
