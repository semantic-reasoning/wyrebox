#include "wyrebox-daemon-connection-server.h"
#include "wyrebox-daemon-connection-session.h"
#include "wyrebox-daemon-socket-listener.h"

#include <gio/gio.h>

typedef struct
{
  grefcount ref_count;
  GMutex connection_mutex;
  GPtrArray *connections;
  WyreboxDaemonRequestAdapter *request_adapter;
} WyreboxDaemonConnectionServerSharedState;

typedef struct
{
  GSocketConnection *connection;
  WyreboxDaemonConnectionServerSharedState *shared_state;
  WyreboxDaemonPeerCredentials peer_credentials;
} WyreboxDaemonConnectionServerSessionRunnerContext;

typedef struct
{
  WyreboxDaemonConnectionServerSharedState *shared_state;
} WyreboxDaemonConnectionServerAcceptContext;

struct _WyreboxDaemonConnectionServer
{
  GObject parent_instance;

  WyreboxDaemonSocketListener *listener;
  WyreboxDaemonConnectionServerSharedState *shared_state;
};

G_DEFINE_TYPE (WyreboxDaemonConnectionServer,
    wyrebox_daemon_connection_server, G_TYPE_OBJECT);

static WyreboxDaemonConnectionServerSharedState
    * wyrebox_daemon_connection_server_shared_state_ref
    (WyreboxDaemonConnectionServerSharedState * state)
{
  g_ref_count_inc (&state->ref_count);
  return state;
}

static void
    wyrebox_daemon_connection_server_shared_state_unref
    (WyreboxDaemonConnectionServerSharedState * state)
{
  if (!g_ref_count_dec (&state->ref_count))
    return;

  g_clear_pointer (&state->connections, g_ptr_array_unref);
  g_clear_object (&state->request_adapter);
  g_mutex_clear (&state->connection_mutex);
  g_free (state);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (WyreboxDaemonConnectionServerSharedState,
    wyrebox_daemon_connection_server_shared_state_unref)
     static WyreboxDaemonConnectionServerSharedState
         *wyrebox_daemon_connection_server_shared_state_new
         (WyreboxDaemonRequestAdapter *request_adapter)
{
  WyreboxDaemonConnectionServerSharedState *state = g_new0
      (WyreboxDaemonConnectionServerSharedState, 1);

  g_ref_count_init (&state->ref_count);
  g_mutex_init (&state->connection_mutex);
  state->connections = g_ptr_array_new_with_free_func (g_object_unref);
  state->request_adapter = g_object_ref (request_adapter);

  return state;
}

static void
    wyrebox_daemon_connection_server_shared_state_add_connection
    (WyreboxDaemonConnectionServerSharedState * state,
    GSocketConnection * connection)
{
  g_mutex_lock (&state->connection_mutex);
  g_ptr_array_add (state->connections, g_object_ref (connection));
  g_mutex_unlock (&state->connection_mutex);
}

static void
    wyrebox_daemon_connection_server_shared_state_remove_connection
    (WyreboxDaemonConnectionServerSharedState * state,
    GSocketConnection * connection)
{
  g_mutex_lock (&state->connection_mutex);
  (void) g_ptr_array_remove (state->connections, connection);
  g_mutex_unlock (&state->connection_mutex);
}

static void
    wyrebox_daemon_connection_server_shared_state_close_connections
    (WyreboxDaemonConnectionServerSharedState * state)
{
  g_autoptr (GPtrArray) snapshot =
      g_ptr_array_new_with_free_func (g_object_unref);

  g_mutex_lock (&state->connection_mutex);
  for (guint i = 0; i < state->connections->len; i++)
    g_ptr_array_add (snapshot, g_object_ref (g_ptr_array_index
            (state->connections, i)));
  g_mutex_unlock (&state->connection_mutex);

  for (guint i = 0; i < snapshot->len; i++) {
    g_autoptr (GError) shutdown_error = NULL;
    GSocketConnection *connection = g_ptr_array_index (snapshot, i);
    GSocket *socket = g_socket_connection_get_socket (connection);

    if (!g_socket_shutdown (socket, TRUE, TRUE, &shutdown_error))
      g_message ("daemon connection failed to shutdown during stop: %s",
          shutdown_error->message);
  }
}

static void
    wyrebox_daemon_connection_server_accept_context_free
    (WyreboxDaemonConnectionServerAcceptContext * context)
{
  if (context == NULL)
    return;

  if (context->shared_state != NULL)
    wyrebox_daemon_connection_server_shared_state_unref (context->shared_state);

  g_free (context);
}

static void
    wyrebox_daemon_connection_server_session_runner_context_free
    (WyreboxDaemonConnectionServerSessionRunnerContext * context)
{
  if (context == NULL)
    return;

  g_clear_object (&context->connection);
  if (context->shared_state != NULL)
    wyrebox_daemon_connection_server_shared_state_unref (context->shared_state);

  g_free (context);
}

static GBytes *
wyrebox_daemon_connection_server_handle_payload (const
    WyreboxDaemonPeerCredentials *peer_credentials, GBytes *request,
    gpointer user_data, GError **error)
{
  WyreboxDaemonConnectionServerSharedState *state = user_data;
  GBytes *response = NULL;

  g_return_val_if_fail (state != NULL, NULL);

  response = wyrebox_daemon_request_adapter_handle_payload (peer_credentials,
      request, state->request_adapter, error);

  return response;
}

static void
wyrebox_daemon_connection_server_run_session (GTask *task,
    gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  WyreboxDaemonConnectionServerSessionRunnerContext *context = task_data;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GError) close_error = NULL;

  (void) source_object;
  (void) cancellable;

  session = wyrebox_daemon_connection_session_new (context->connection,
      &context->peer_credentials,
      wyrebox_daemon_connection_server_handle_payload,
      wyrebox_daemon_connection_server_shared_state_ref (context->shared_state),
      (GDestroyNotify) wyrebox_daemon_connection_server_shared_state_unref);

  if (!wyrebox_daemon_connection_session_process_payloads (session, &error))
    g_message ("daemon connection session failed: %s", error->message);

  if (!g_io_stream_close (G_IO_STREAM (context->connection), NULL,
          &close_error))
    g_message ("daemon connection failed to close: %s", close_error->message);

  wyrebox_daemon_connection_server_shared_state_remove_connection
      (context->shared_state, context->connection);

  g_task_return_boolean (task, TRUE);
}

static void
wyrebox_daemon_connection_server_handle_connection (WyreboxDaemonSocketListener
    *listener, GSocketConnection *connection,
    const WyreboxDaemonPeerCredentials *credentials, gpointer user_data)
{
  g_autoptr (GTask) task = NULL;
  WyreboxDaemonConnectionServerSessionRunnerContext *runner_context = NULL;
  WyreboxDaemonConnectionServerAcceptContext *context = user_data;

  (void) listener;

  if (user_data == NULL || connection == NULL || credentials == NULL)
    return;

  runner_context =
      g_new0 (WyreboxDaemonConnectionServerSessionRunnerContext, 1);
  runner_context->connection = g_object_ref (connection);
  runner_context->shared_state =
      wyrebox_daemon_connection_server_shared_state_ref (context->shared_state);
  runner_context->peer_credentials = *credentials;

  wyrebox_daemon_connection_server_shared_state_add_connection
      (context->shared_state, connection);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_steal_pointer (&runner_context),
      (GDestroyNotify)
      wyrebox_daemon_connection_server_session_runner_context_free);
  g_task_run_in_thread (task, wyrebox_daemon_connection_server_run_session);
}

static void
wyrebox_daemon_connection_server_finalize (GObject *object)
{
  WyreboxDaemonConnectionServer *self =
      WYREBOX_DAEMON_CONNECTION_SERVER (object);
  g_autoptr (GError) stop_error = NULL;

  if (self->listener != NULL) {
    (void) wyrebox_daemon_socket_listener_stop (self->listener, &stop_error);
    wyrebox_daemon_socket_listener_set_connection_handler (self->listener,
        NULL, NULL, NULL);
  }

  if (self->shared_state != NULL) {
    wyrebox_daemon_connection_server_shared_state_close_connections
        (self->shared_state);
    wyrebox_daemon_connection_server_shared_state_unref (self->shared_state);
    self->shared_state = NULL;
  }

  g_clear_object (&self->listener);

  G_OBJECT_CLASS (wyrebox_daemon_connection_server_parent_class)->finalize
      (object);
}

static void
wyrebox_daemon_connection_server_class_init (WyreboxDaemonConnectionServerClass
    *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_connection_server_finalize;
}

static void
wyrebox_daemon_connection_server_init (WyreboxDaemonConnectionServer *self)
{
  (void) self;
}

WyreboxDaemonConnectionServer *
wyrebox_daemon_connection_server_new (const char *socket_path,
    WyreboxDaemonRequestAdapter *request_adapter)
{
  WyreboxDaemonConnectionServerAcceptContext *context = NULL;

  g_return_val_if_fail (socket_path != NULL, NULL);
  g_return_val_if_fail (*socket_path != '\0', NULL);
  g_return_val_if_fail (WYREBOX_IS_DAEMON_REQUEST_ADAPTER (request_adapter),
      NULL);

  WyreboxDaemonConnectionServer *self =
      g_object_new (WYREBOX_TYPE_DAEMON_CONNECTION_SERVER, NULL);
  self->listener = wyrebox_daemon_socket_listener_new (socket_path);
  self->shared_state =
      wyrebox_daemon_connection_server_shared_state_new (request_adapter);

  context = g_new0 (WyreboxDaemonConnectionServerAcceptContext, 1);
  context->shared_state =
      wyrebox_daemon_connection_server_shared_state_ref (self->shared_state);
  wyrebox_daemon_socket_listener_set_connection_handler (self->listener,
      wyrebox_daemon_connection_server_handle_connection,
      g_steal_pointer (&context),
      (GDestroyNotify) wyrebox_daemon_connection_server_accept_context_free);

  return self;
}

gboolean
wyrebox_daemon_connection_server_start (WyreboxDaemonConnectionServer *self,
    GError **error)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_CONNECTION_SERVER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return wyrebox_daemon_socket_listener_start (self->listener, error);
}

gboolean
wyrebox_daemon_connection_server_stop (WyreboxDaemonConnectionServer *self,
    GError **error)
{
  gboolean stopped = FALSE;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_CONNECTION_SERVER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  stopped = wyrebox_daemon_socket_listener_stop (self->listener, error);
  if (self->shared_state != NULL)
    wyrebox_daemon_connection_server_shared_state_close_connections
        (self->shared_state);

  return stopped;
}
