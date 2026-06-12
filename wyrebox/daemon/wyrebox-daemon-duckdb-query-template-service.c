#include "wyrebox-daemon-duckdb-query-template-service.h"

#include <gio/gio.h>

struct _WyreboxDaemonDuckDBQueryTemplateService
{
  GObject parent_instance;

  WyreboxDaemonDuckDBQueryTemplateServiceFunc func;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
};

G_DEFINE_TYPE (WyreboxDaemonDuckDBQueryTemplateService,
    wyrebox_daemon_duckdb_query_template_service, G_TYPE_OBJECT);

static gboolean
caller_can_query_template (const char *caller_identity)
{
  return g_strcmp0 (caller_identity, "skill") == 0
      || g_strcmp0 (caller_identity, "agent") == 0;
}

static gboolean
authorize_duckdb_query_template_identity (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    GError **error)
{
  if (!caller_can_query_template (identity->caller_identity)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to query duckdb query templates");
    return FALSE;
  }

  if (identity->account_identity == NULL || *identity->account_identity == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller account scope is required for duckdb query template");
    return FALSE;
  }

  if (g_strcmp0 (identity->account_identity, request->scope_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for duckdb query template account scope");
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_duckdb_query_template_chunk (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const WyreboxDaemonStreamChunkFrame *chunk, GError **error)
{
  if (chunk->request_id == NULL || *chunk->request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk request_id is required");
    return FALSE;
  }

  if (g_strcmp0 (chunk->request_id, identity->request_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk request_id must match request "
        "envelope");
    return FALSE;
  }

  if (chunk->message_id != NULL && *chunk->message_id != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk must not contain message_id");
    return FALSE;
  }

  if (chunk->query_id == NULL || *chunk->query_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk query_id is required");
    return FALSE;
  }

  if (g_strcmp0 (chunk->query_id, request->query_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk query_id must match request");
    return FALSE;
  }

  if (chunk->correlation_id != NULL
      && g_strcmp0 (chunk->correlation_id, identity->correlation_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk correlation_id must match request "
        "envelope");
    return FALSE;
  }

  return TRUE;
}

static void
wyrebox_daemon_duckdb_query_template_service_finalize (GObject *object)
{
  WyreboxDaemonDuckDBQueryTemplateService *self =
      WYREBOX_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE (object);

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  G_OBJECT_CLASS
      (wyrebox_daemon_duckdb_query_template_service_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_duckdb_query_template_service_class_init
    (WyreboxDaemonDuckDBQueryTemplateServiceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize =
      wyrebox_daemon_duckdb_query_template_service_finalize;
}

static void
    wyrebox_daemon_duckdb_query_template_service_init
    (WyreboxDaemonDuckDBQueryTemplateService * self)
{
  (void) self;
}

WyreboxDaemonDuckDBQueryTemplateService
    * wyrebox_daemon_duckdb_query_template_service_new
    (WyreboxDaemonDuckDBQueryTemplateServiceFunc func, gpointer user_data,
    GDestroyNotify user_data_destroy) {
  g_return_val_if_fail (func != NULL, NULL);

  WyreboxDaemonDuckDBQueryTemplateService *self =
      g_object_new (WYREBOX_TYPE_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE, NULL);

  self->func = func;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;

  return self;
}

gboolean
    wyrebox_daemon_duckdb_query_template_service_handle_identity
    (WyreboxDaemonDuckDBQueryTemplateService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonStreamChunkFrame) response_chunk = { 0 };
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE
      (self), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!authorize_duckdb_query_template_identity (identity, request, error))
    return FALSE;

  if (!self->func (identity, request, &chunk, self->user_data, &local_error)) {
    if (local_error == NULL) {
      g_set_error (&local_error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "duckdb query template service failed without error detail");
    }
    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }

  if (!validate_duckdb_query_template_chunk (identity, request, &chunk, error))
    return FALSE;

  if (!wyrebox_daemon_stream_chunk_frame_init (&response_chunk,
          identity->request_id,
          NULL, request->query_id, identity->correlation_id,
          chunk.chunk_index, chunk.bytes, chunk.end_of_stream, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_stream_chunk (out_frame,
      &response_chunk, error);
}
