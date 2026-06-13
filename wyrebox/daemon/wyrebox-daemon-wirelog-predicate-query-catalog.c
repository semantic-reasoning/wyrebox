#include "wyrebox-daemon-wirelog-predicate-query-catalog.h"

#include <gio/gio.h>

static const WyreboxDaemonWirelogPredicateQueryDescriptor catalog[] = {
  {
        "show_in_virtual_folder.v1",
        "show_in_virtual_folder",
        "account_id",
        "stream-chunk.wirelog-predicate.show-in-virtual-folder.v1",
      0},
};

static gboolean
has_control_character (const char *value)
{
  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor))
      return TRUE;
  }

  return FALSE;
}

static gsize
count_bindings (gchar **bindings)
{
  gsize count = 0;

  if (bindings == NULL)
    return 0;

  while (bindings[count] != NULL)
    count++;

  return count;
}

static const WyreboxDaemonWirelogPredicateQueryDescriptor *
lookup_predicate (const char *predicate_id)
{
  for (gsize i = 0; i < G_N_ELEMENTS (catalog); i++) {
    if (g_strcmp0 (catalog[i].predicate_id, predicate_id) == 0)
      return &catalog[i];
  }

  return NULL;
}

gboolean
    wyrebox_daemon_wirelog_predicate_query_catalog_validate
    (WyreboxDaemonClientIdentityClass client_class,
    const char *caller_account_id,
    const WyreboxDaemonWirelogPredicateQueryRequest * request,
    const WyreboxDaemonWirelogPredicateQueryDescriptor ** out_descriptor,
    GError ** error)
{
  const WyreboxDaemonWirelogPredicateQueryDescriptor *descriptor = NULL;
  gsize binding_count = 0;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_descriptor != NULL)
    *out_descriptor = NULL;

  if (!wyrebox_daemon_client_identity_can_query_controlled_views (client_class)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to query wirelog predicates");
    return FALSE;
  }

  if (request == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "wirelog predicate query request is required");
    return FALSE;
  }

  if (caller_account_id == NULL || *caller_account_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller account scope is required for wirelog predicate query");
    return FALSE;
  }

  if (request->scope_id == NULL || *request->scope_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "wirelog predicate query account scope is required");
    return FALSE;
  }

  if (g_strcmp0 (caller_account_id, request->scope_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for wirelog predicate query account scope");
    return FALSE;
  }

  descriptor = lookup_predicate (request->predicate_id);
  if (descriptor == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unknown wirelog predicate query '%s'",
        request->predicate_id != NULL ? request->predicate_id : "(null)");
    return FALSE;
  }

  binding_count = count_bindings (request->bindings);
  for (gsize i = 0; i < binding_count; i++) {
    const char *binding = request->bindings[i];

    if (binding == NULL || *binding == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "wirelog predicate query binding is required");
      return FALSE;
    }

    if (has_control_character (binding)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "wirelog predicate query binding must not contain control characters");
      return FALSE;
    }
  }

  if (binding_count != descriptor->n_bindings) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "wirelog predicate query '%s' expects %" G_GSIZE_FORMAT " binding(s)",
        descriptor->predicate_id, descriptor->n_bindings);
    return FALSE;
  }

  if (out_descriptor != NULL)
    *out_descriptor = descriptor;

  return TRUE;
}
