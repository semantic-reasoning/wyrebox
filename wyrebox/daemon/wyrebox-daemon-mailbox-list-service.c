#include "wyrebox-daemon-mailbox-list-service.h"

#include <gio/gio.h>

struct _WyreboxDaemonMailboxListService
{
  GObject parent_instance;

  WyreboxDaemonMailboxListServiceFunc func;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
};

G_DEFINE_TYPE (WyreboxDaemonMailboxListService,
    wyrebox_daemon_mailbox_list_service, G_TYPE_OBJECT);

static gboolean
caller_can_list_mailboxes (const char *caller_identity)
{
  return g_strcmp0 (caller_identity, "dovecot") == 0;
}

static gboolean
authorize_mailbox_list_identity (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request, GError **error)
{
  if (!caller_can_list_mailboxes (identity->caller_identity)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to list mailboxes");
    return FALSE;
  }

  if (identity->account_identity == NULL ||
      g_strcmp0 (identity->account_identity, request->account_identity) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for mailbox LIST account scope");
    return FALSE;
  }

  return TRUE;
}

static void
wyrebox_daemon_mailbox_list_service_finalize (GObject *object)
{
  WyreboxDaemonMailboxListService *self =
      WYREBOX_DAEMON_MAILBOX_LIST_SERVICE (object);

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  G_OBJECT_CLASS (wyrebox_daemon_mailbox_list_service_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_mailbox_list_service_class_init
    (WyreboxDaemonMailboxListServiceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_mailbox_list_service_finalize;
}

static void
wyrebox_daemon_mailbox_list_service_init (WyreboxDaemonMailboxListService *self)
{
}

WyreboxDaemonMailboxListService *
wyrebox_daemon_mailbox_list_service_new (WyreboxDaemonMailboxListServiceFunc
    func, gpointer user_data, GDestroyNotify user_data_destroy)
{
  g_return_val_if_fail (func != NULL, NULL);

  WyreboxDaemonMailboxListService *self =
      g_object_new (WYREBOX_TYPE_DAEMON_MAILBOX_LIST_SERVICE, NULL);

  self->func = func;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;

  return self;
}

gboolean
    wyrebox_daemon_mailbox_list_service_handle_identity
    (WyreboxDaemonMailboxListService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonMailboxListRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };

  g_return_val_if_fail (WYREBOX_IS_DAEMON_MAILBOX_LIST_SERVICE (self), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!authorize_mailbox_list_identity (identity, request, error))
    return FALSE;

  if (!self->func (identity, request, &result, self->user_data, error)) {
    if (error != NULL && *error == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "mailbox LIST service failed without error detail");
    }
    return FALSE;
  }

  return wyrebox_daemon_response_frame_init_mailbox_list (out_frame,
      identity->request_id, identity->correlation_id, &result, error);
}
