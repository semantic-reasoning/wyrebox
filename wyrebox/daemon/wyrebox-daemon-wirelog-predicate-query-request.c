#include "wyrebox-daemon-wirelog-predicate-query-request.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "wirelog predicate query %s is required", field_name);
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
          "wirelog predicate query %s must not contain control characters",
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
          "wirelog predicate query %s contains invalid characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
copy_bindings (const char *const *bindings, gchar ***out_bindings,
    GError **error)
{
  g_auto (GStrv) copied = NULL;
  gsize binding_count = 0;

  g_return_val_if_fail (out_bindings != NULL, FALSE);

  *out_bindings = NULL;

  if (bindings == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "wirelog predicate query bindings is required");
    return FALSE;
  }

  while (bindings[binding_count] != NULL) {
    const char *binding = bindings[binding_count];

    if (binding == NULL || *binding == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "wirelog predicate query binding is required");
      return FALSE;
    }

    if (!validate_no_control_characters (binding, "binding", error))
      return FALSE;

    binding_count++;
  }

  copied = g_new0 (gchar *, binding_count + 1);

  for (gsize i = 0; i < binding_count; i++)
    copied[i] = g_strdup (bindings[i]);

  *out_bindings = g_steal_pointer (&copied);
  return TRUE;
}

void wyrebox_daemon_wirelog_predicate_query_request_clear
    (WyreboxDaemonWirelogPredicateQueryRequest * request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->query_id, g_free);
  g_clear_pointer (&request->predicate_id, g_free);
  g_clear_pointer (&request->scope_id, g_free);
  g_clear_pointer (&request->bindings, g_strfreev);
}

gboolean
    wyrebox_daemon_wirelog_predicate_query_request_init
    (WyreboxDaemonWirelogPredicateQueryRequest * request, const char *query_id,
    const char *predicate_id, const char *scope_id, const char *const *bindings,
    GError ** error)
{
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) next = { 0 };
  g_auto (GStrv) next_bindings = NULL;

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_conservative_token (query_id, "query_id", error))
    return FALSE;

  if (!validate_conservative_token (predicate_id, "predicate_id", error))
    return FALSE;

  if (!validate_required_text (scope_id, "scope_id", error))
    return FALSE;

  if (!validate_no_control_characters (scope_id, "scope_id", error))
    return FALSE;

  if (!copy_bindings (bindings, &next_bindings, error))
    return FALSE;

  next.query_id = g_strdup (query_id);
  next.predicate_id = g_strdup (predicate_id);
  next.scope_id = g_strdup (scope_id);
  next.bindings = g_steal_pointer (&next_bindings);

  wyrebox_daemon_wirelog_predicate_query_request_clear (request);
  *request = next;
  next.query_id = NULL;
  next.predicate_id = NULL;
  next.scope_id = NULL;
  next.bindings = NULL;

  return TRUE;
}
