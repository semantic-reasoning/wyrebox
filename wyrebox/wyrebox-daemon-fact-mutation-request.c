#include "wyrebox-daemon-fact-mutation-request.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
is_supported_mutation (WyreboxDaemonFactMutationKind mutation)
{
  return mutation == WYREBOX_DAEMON_FACT_MUTATION_INSERT ||
      mutation == WYREBOX_DAEMON_FACT_MUTATION_RETRACT;
}

static gboolean
is_predicate_start (char value)
{
  return g_ascii_islower (value) || value == '_';
}

static gboolean
is_predicate_char (char value)
{
  return g_ascii_isalnum (value) || value == '_';
}

static gboolean
validate_predicate_id (const char *predicate_id, GError **error)
{
  if (predicate_id == NULL || *predicate_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact mutation predicate_id is required");
    return FALSE;
  }

  if (!is_predicate_start (predicate_id[0])) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation predicate_id must start with lowercase ASCII or underscore");
    return FALSE;
  }

  for (const char *cursor = predicate_id + 1; *cursor != '\0'; cursor++) {
    if (!is_predicate_char (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "fact mutation predicate_id contains an unsupported character");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_required_text (const char *value,
    const char *field_name, GError **error)
{
  if (value != NULL && *value != '\0')
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_ARGUMENT, "fact mutation %s is required", field_name);
  return FALSE;
}

static gboolean
validate_arguments (const char *const *arguments, GError **error)
{
  if (arguments == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation arguments vector is required");
    return FALSE;
  }

  for (guint index = 0; arguments[index] != NULL; index++) {
    if (arguments[index][0] == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "fact mutation arguments must not contain empty values");
      return FALSE;
    }
  }

  return TRUE;
}

void
wyrebox_daemon_fact_mutation_request_clear (WyreboxDaemonFactMutationRequest
    *request)
{
  if (request == NULL)
    return;

  request->mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT;
  g_clear_pointer (&request->predicate_id, g_free);
  g_clear_pointer (&request->scope_id, g_free);
  g_clear_pointer (&request->arguments, g_strfreev);
}

gboolean
wyrebox_daemon_fact_mutation_request_init (WyreboxDaemonFactMutationRequest
    *request, WyreboxDaemonFactMutationKind mutation, const char *predicate_id,
    const char *scope_id, const char *const *arguments, GError **error)
{
  g_auto (WyreboxDaemonFactMutationRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!is_supported_mutation (mutation)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "unsupported fact mutation kind");
    return FALSE;
  }

  if (!validate_predicate_id (predicate_id, error))
    return FALSE;

  if (!validate_required_text (scope_id, "scope_id", error))
    return FALSE;

  if (!validate_arguments (arguments, error))
    return FALSE;

  next.mutation = mutation;
  next.predicate_id = g_strdup (predicate_id);
  next.scope_id = g_strdup (scope_id);
  next.arguments = g_strdupv ((char **) arguments);

  wyrebox_daemon_fact_mutation_request_clear (request);
  *request = next;
  memset (&next, 0, sizeof (next));

  return TRUE;
}
