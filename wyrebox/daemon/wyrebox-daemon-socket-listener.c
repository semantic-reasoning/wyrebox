#include "wyrebox-daemon-socket-listener.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <sys/stat.h>

#define ACCEPT_RETRY_DELAY_MS 250

struct _WyreboxDaemonSocketListener
{
  GObject parent_instance;

  char *socket_path;
  GSocketListener *listener;
  GCancellable *accept_cancellable;
  guint accept_retry_source_id;
  gboolean started;
  guint accept_generation;

  WyreboxDaemonSocketListenerConnectionHandler connection_handler;
  gpointer connection_handler_data;
  GDestroyNotify connection_handler_destroy_notify;

  gboolean created_inode_valid;
  dev_t created_device;
  ino_t created_inode;
};

typedef struct
{
  GWeakRef listener_ref;
  guint generation;
  guint source_id;
} AcceptLoopData;

G_DEFINE_TYPE (WyreboxDaemonSocketListener, wyrebox_daemon_socket_listener,
    G_TYPE_OBJECT);

static void arm_accept_loop (WyreboxDaemonSocketListener * self);

static AcceptLoopData *
accept_loop_data_new (WyreboxDaemonSocketListener *self, guint generation)
{
  AcceptLoopData *data = g_new0 (AcceptLoopData, 1);

  g_weak_ref_init (&data->listener_ref, self);
  data->generation = generation;

  return data;
}

static void
accept_loop_data_free (AcceptLoopData *data)
{
  g_weak_ref_clear (&data->listener_ref);
  g_free (data);
}

static gboolean
stat_socket_path (const char *socket_path,
    dev_t *out_device, ino_t *out_inode, GError **error)
{
  struct stat socket_stat = { 0 };

  if (lstat (socket_path, &socket_stat) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to stat daemon socket '%s': %s",
        socket_path, g_strerror (saved_errno));
    return FALSE;
  }

  if (!S_ISSOCK (socket_stat.st_mode)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "daemon socket path '%s' is not a Unix-domain socket", socket_path);
    return FALSE;
  }

  *out_device = socket_stat.st_dev;
  *out_inode = socket_stat.st_ino;
  return TRUE;
}

static gboolean
socket_path_matches_inode (const char *socket_path, dev_t device, ino_t inode)
{
  struct stat socket_stat = { 0 };

  if (lstat (socket_path, &socket_stat) != 0)
    return FALSE;

  return S_ISSOCK (socket_stat.st_mode)
      && socket_stat.st_dev == device && socket_stat.st_ino == inode;
}

static gboolean
socket_path_matches_created_inode (WyreboxDaemonSocketListener *self)
{
  if (!self->created_inode_valid)
    return FALSE;

  return socket_path_matches_inode (self->socket_path,
      self->created_device, self->created_inode);
}

static void
unlink_created_socket_path (WyreboxDaemonSocketListener *self)
{
  if (socket_path_matches_created_inode (self))
    (void) g_unlink (self->socket_path);

  self->created_inode_valid = FALSE;
}

static void
cancel_accept_retry (WyreboxDaemonSocketListener *self)
{
  if (self->accept_retry_source_id == 0)
    return;

  g_source_remove (self->accept_retry_source_id);
  self->accept_retry_source_id = 0;
}

static gboolean
accept_error_is_expected_after_stop (const GError *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CLOSED);
}

static const char *
error_message_or_unknown (const GError *error)
{
  return error != NULL ? error->message : "unknown error";
}

static gboolean
retry_accept_loop (gpointer user_data)
{
  AcceptLoopData *data = user_data;
  g_autoptr (WyreboxDaemonSocketListener) self = NULL;

  self = g_weak_ref_get (&data->listener_ref);
  if (self == NULL)
    return G_SOURCE_REMOVE;

  if (self->accept_retry_source_id == data->source_id)
    self->accept_retry_source_id = 0;

  if (self->started
      && self->accept_generation == data->generation && self->listener != NULL)
    arm_accept_loop (self);

  return G_SOURCE_REMOVE;
}

static void
schedule_accept_retry (WyreboxDaemonSocketListener *self, guint generation)
{
  AcceptLoopData *data = NULL;

  if (!self->started
      || self->accept_generation != generation
      || self->listener == NULL || self->accept_retry_source_id != 0)
    return;

  data = accept_loop_data_new (self, generation);
  self->accept_retry_source_id = g_timeout_add_full (G_PRIORITY_DEFAULT,
      ACCEPT_RETRY_DELAY_MS, retry_accept_loop, data,
      (GDestroyNotify) accept_loop_data_free);
  data->source_id = self->accept_retry_source_id;
}

static void
accepted_connection_ready (GObject *source_object,
    GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GSocketConnection) connection = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) self = NULL;
  g_autoptr (GError) credentials_error = NULL;
  AcceptLoopData *data = user_data;
  WyreboxDaemonPeerCredentials credentials = { 0 };

  connection =
      g_socket_listener_accept_finish (G_SOCKET_LISTENER (source_object),
      result, NULL, &error);
  self = g_weak_ref_get (&data->listener_ref);

  if (connection == NULL) {
    if (!accept_error_is_expected_after_stop (error)) {
      if (self != NULL
          && self->started && self->accept_generation == data->generation) {
        g_warning ("daemon socket accept failed: %s; retrying in %u ms",
            error_message_or_unknown (error), ACCEPT_RETRY_DELAY_MS);
        schedule_accept_retry (self, data->generation);
      } else {
        g_warning ("daemon socket accept failed: %s",
            error_message_or_unknown (error));
      }
    }

    accept_loop_data_free (data);
    return;
  }

  if (self == NULL
      || !self->started || self->accept_generation != data->generation) {
    accept_loop_data_free (data);
    return;
  }

  if (!wyrebox_daemon_peer_credentials_from_socket
      (g_socket_connection_get_socket (connection), &credentials,
          &credentials_error)) {
    g_warning ("failed to capture daemon peer credentials: %s",
        credentials_error->message);
  } else if (self->connection_handler != NULL) {
    self->connection_handler (self,
        connection, &credentials, self->connection_handler_data);
  }

  if (self->started && self->accept_generation == data->generation)
    arm_accept_loop (self);

  accept_loop_data_free (data);
}

static void
arm_accept_loop (WyreboxDaemonSocketListener *self)
{
  AcceptLoopData *data = NULL;

  if (!self->started || self->listener == NULL)
    return;

  data = accept_loop_data_new (self, self->accept_generation);

  g_socket_listener_accept_async (self->listener,
      self->accept_cancellable, accepted_connection_ready, data);
}

static gboolean
ensure_socket_parent_dir (const char *socket_path, GError **error)
{
  g_autofree char *parent_dir = g_path_get_dirname (socket_path);
  gboolean parent_existed = g_file_test (parent_dir, G_FILE_TEST_EXISTS);

  if (g_mkdir_with_parents (parent_dir, 0750) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to create daemon socket directory '%s': %s",
        parent_dir, g_strerror (saved_errno));
    return FALSE;
  }

  if (parent_existed)
    return TRUE;

  if (g_chmod (parent_dir, 0750) == 0)
    return TRUE;

  int saved_errno = errno;

  g_set_error (error,
      G_IO_ERROR,
      g_io_error_from_errno (saved_errno),
      "failed to set daemon socket directory mode for '%s': %s",
      parent_dir, g_strerror (saved_errno));
  return FALSE;
}

static gboolean
connect_error_indicates_stale_socket (const GError *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static gboolean
unlink_stale_socket_path (const char *socket_path,
    dev_t device, ino_t inode, GError **error)
{
  struct stat socket_stat = { 0 };

  if (lstat (socket_path, &socket_stat) != 0) {
    int saved_errno = errno;

    if (saved_errno == ENOENT)
      return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to stat stale daemon socket '%s': %s",
        socket_path, g_strerror (saved_errno));
    return FALSE;
  }

  if (!S_ISSOCK (socket_stat.st_mode)
      || socket_stat.st_dev != device || socket_stat.st_ino != inode) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_EXISTS,
        "daemon socket path '%s' changed during stale socket recovery",
        socket_path);
    return FALSE;
  }

  if (g_unlink (socket_path) == 0)
    return TRUE;

  int saved_errno = errno;

  if (saved_errno == ENOENT)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      g_io_error_from_errno (saved_errno),
      "failed to unlink stale daemon socket '%s': %s",
      socket_path, g_strerror (saved_errno));
  return FALSE;
}

static gboolean
recover_stale_socket_path_before_bind (const char *socket_path, GError **error)
{
  g_autoptr (GError) connect_error = NULL;
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  struct stat socket_stat = { 0 };

  if (lstat (socket_path, &socket_stat) != 0) {
    int saved_errno = errno;

    if (saved_errno == ENOENT)
      return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to stat daemon socket path '%s': %s",
        socket_path, g_strerror (saved_errno));
    return FALSE;
  }

  if (!S_ISSOCK (socket_stat.st_mode))
    return TRUE;

  client = g_socket_client_new ();
  address = g_unix_socket_address_new (socket_path);
  connection = g_socket_client_connect (client,
      G_SOCKET_CONNECTABLE (address), NULL, &connect_error);

  if (connection != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_ADDRESS_IN_USE,
        "daemon socket path '%s' is already served by a live listener",
        socket_path);
    return FALSE;
  }

  if (!connect_error_indicates_stale_socket (connect_error)) {
    g_propagate_prefixed_error (error,
        g_steal_pointer (&connect_error),
        "failed to probe daemon socket '%s': ", socket_path);
    return FALSE;
  }

  return unlink_stale_socket_path (socket_path,
      socket_stat.st_dev, socket_stat.st_ino, error);
}

static gboolean
chmod_socket_path (const char *socket_path, GError **error)
{
  if (g_chmod (socket_path, 0660) == 0)
    return TRUE;

  int saved_errno = errno;

  g_set_error (error,
      G_IO_ERROR,
      g_io_error_from_errno (saved_errno),
      "failed to set daemon socket mode for '%s': %s",
      socket_path, g_strerror (saved_errno));
  return FALSE;
}

static void
wyrebox_daemon_socket_listener_finalize (GObject *object)
{
  WyreboxDaemonSocketListener *self = WYREBOX_DAEMON_SOCKET_LISTENER (object);
  g_autoptr (GError) local_error = NULL;

  (void) wyrebox_daemon_socket_listener_stop (self, &local_error);
  cancel_accept_retry (self);

  if (self->connection_handler_destroy_notify != NULL)
    self->connection_handler_destroy_notify (self->connection_handler_data);

  g_clear_object (&self->accept_cancellable);
  g_clear_pointer (&self->socket_path, g_free);

  G_OBJECT_CLASS (wyrebox_daemon_socket_listener_parent_class)->finalize
      (object);
}

static void
wyrebox_daemon_socket_listener_class_init (WyreboxDaemonSocketListenerClass
    *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_socket_listener_finalize;
}

static void
wyrebox_daemon_socket_listener_init (WyreboxDaemonSocketListener *self)
{
}

WyreboxDaemonSocketListener *
wyrebox_daemon_socket_listener_new (const char *socket_path)
{
  WyreboxDaemonSocketListener *self = NULL;

  g_return_val_if_fail (socket_path != NULL, NULL);
  g_return_val_if_fail (*socket_path != '\0', NULL);

  self = g_object_new (WYREBOX_TYPE_DAEMON_SOCKET_LISTENER, NULL);
  self->socket_path = g_strdup (socket_path);

  return self;
}

const char *
wyrebox_daemon_socket_listener_get_socket_path (WyreboxDaemonSocketListener
    *self)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_SOCKET_LISTENER (self), NULL);

  return self->socket_path;
}

gboolean
wyrebox_daemon_socket_listener_is_started (WyreboxDaemonSocketListener *self)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_SOCKET_LISTENER (self), FALSE);

  return self->started;
}

void wyrebox_daemon_socket_listener_set_connection_handler
    (WyreboxDaemonSocketListener * self,
    WyreboxDaemonSocketListenerConnectionHandler handler, gpointer user_data,
    GDestroyNotify destroy_notify)
{
  g_return_if_fail (WYREBOX_IS_DAEMON_SOCKET_LISTENER (self));

  if (self->connection_handler_destroy_notify != NULL)
    self->connection_handler_destroy_notify (self->connection_handler_data);

  self->connection_handler = handler;
  self->connection_handler_data = user_data;
  self->connection_handler_destroy_notify = destroy_notify;
}

gboolean
wyrebox_daemon_socket_listener_start (WyreboxDaemonSocketListener *self,
    GError **error)
{
  g_autoptr (GSocketListener) listener = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  dev_t created_device = 0;
  ino_t created_inode = 0;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_SOCKET_LISTENER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->started) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_EXISTS,
        "daemon socket listener for '%s' is already started",
        self->socket_path);
    return FALSE;
  }

  if (!ensure_socket_parent_dir (self->socket_path, error))
    return FALSE;

  if (!recover_stale_socket_path_before_bind (self->socket_path, error))
    return FALSE;

  listener = g_socket_listener_new ();
  address = g_unix_socket_address_new (self->socket_path);

  if (!g_socket_listener_add_address (listener,
          address,
          G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, error))
    return FALSE;

  if (!stat_socket_path (self->socket_path,
          &created_device, &created_inode, error)) {
    g_socket_listener_close (listener);
    return FALSE;
  }

  self->created_inode_valid = TRUE;
  self->created_device = created_device;
  self->created_inode = created_inode;

  if (!chmod_socket_path (self->socket_path, error)) {
    g_socket_listener_close (listener);
    unlink_created_socket_path (self);
    return FALSE;
  }

  if (!socket_path_matches_created_inode (self)) {
    g_socket_listener_close (listener);
    self->created_inode_valid = FALSE;
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_EXISTS,
        "daemon socket path '%s' changed during listener startup",
        self->socket_path);
    return FALSE;
  }

  self->listener = g_steal_pointer (&listener);
  self->accept_cancellable = g_cancellable_new ();
  self->started = TRUE;
  self->accept_generation++;
  arm_accept_loop (self);

  return TRUE;
}

gboolean
wyrebox_daemon_socket_listener_stop (WyreboxDaemonSocketListener *self,
    GError **error)
{
  g_return_val_if_fail (WYREBOX_IS_DAEMON_SOCKET_LISTENER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!self->started) {
    cancel_accept_retry (self);
    return TRUE;
  }

  self->started = FALSE;
  self->accept_generation++;
  cancel_accept_retry (self);
  g_cancellable_cancel (self->accept_cancellable);
  g_socket_listener_close (self->listener);
  g_clear_object (&self->listener);
  g_clear_object (&self->accept_cancellable);

  unlink_created_socket_path (self);

  return TRUE;
}
