#include "wyrebox-daemon-fact-mutation-request.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
is_supported_mutation (WyreboxDaemonFactMutationKind mutation)
{
  return wyrebox_daemon_fact_mutation_kind_to_wire_name (mutation) != NULL;
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
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "fact mutation %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
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

    for (const char *cursor = arguments[index]; *cursor != '\0'; cursor++) {
      if (g_ascii_iscntrl (*cursor)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "fact mutation arguments must not contain control characters");
        return FALSE;
      }
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

const char *
wyrebox_daemon_fact_mutation_kind_to_wire_name (WyreboxDaemonFactMutationKind
    mutation)
{
  switch (mutation) {
    case WYREBOX_DAEMON_FACT_MUTATION_INSERT:
      return "insert";
    case WYREBOX_DAEMON_FACT_MUTATION_RETRACT:
      return "retract";
    default:
      return NULL;
  }
}

gboolean
wyrebox_daemon_fact_mutation_kind_from_wire_name (const char *wire_name,
    WyreboxDaemonFactMutationKind *mutation, GError **error)
{
  g_return_val_if_fail (mutation != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (g_strcmp0 (wire_name, "insert") == 0) {
    *mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT;
    return TRUE;
  }

  if (g_strcmp0 (wire_name, "retract") == 0) {
    *mutation = WYREBOX_DAEMON_FACT_MUTATION_RETRACT;
    return TRUE;
  }

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_ARGUMENT, "unsupported fact mutation wire name");
  return FALSE;
}

gboolean
wyrebox_daemon_fact_mutation_to_event (WyreboxDaemonFactMutationKind mutation,
    WyreboxJournalEventType *event_type, GError **error)
{
  g_return_val_if_fail (event_type != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  switch (mutation) {
    case WYREBOX_DAEMON_FACT_MUTATION_INSERT:
      *event_type = WYREBOX_JOURNAL_EVENT_FACT_INSERTED;
      return TRUE;
    case WYREBOX_DAEMON_FACT_MUTATION_RETRACT:
      *event_type = WYREBOX_JOURNAL_EVENT_FACT_RETRACTED;
      return TRUE;
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "unsupported fact mutation journal event type");
      return FALSE;
  }
}

gboolean
wyrebox_daemon_fact_mutation_request_get_event (const
    WyreboxDaemonFactMutationRequest *request,
    WyreboxJournalEventType *event_type, GError **error)
{
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (event_type != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (request->predicate_id == NULL || request->scope_id == NULL ||
      request->arguments == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation request is not initialized");
    return FALSE;
  }

  return wyrebox_daemon_fact_mutation_to_event (request->mutation, event_type,
      error);
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
