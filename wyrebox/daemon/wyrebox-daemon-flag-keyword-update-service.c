#include "wyrebox-daemon-flag-keyword-update-service.h"

#include <gio/gio.h>

struct _WyreboxDaemonFlagKeywordUpdateService
{
  GObject parent_instance;

  WyreboxDaemonFlagKeywordUpdateServiceFunc func;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
};

G_DEFINE_TYPE (WyreboxDaemonFlagKeywordUpdateService,
    wyrebox_daemon_flag_keyword_update_service, G_TYPE_OBJECT);

static gboolean
caller_can_update_flags (const char *caller_identity)
{
  return g_strcmp0 (caller_identity, "dovecot") == 0;
}

static gboolean
authorize_flag_keyword_update_identity (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonFlagKeywordUpdateRequest *request,
    GError **error)
{
  if (!caller_can_update_flags (identity->caller_identity)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to update flags or keywords");
    return FALSE;
  }

  if (identity->account_identity == NULL
      || g_strcmp0 (identity->account_identity,
          request->account_identity) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for flag keyword update account scope");
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_flag_keyword_update_receipt (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonSuccessReceipt *receipt, GError **error)
{
  if (receipt->request_id == NULL ||
      g_strcmp0 (receipt->request_id, identity->request_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "flag keyword update receipt request_id must match request envelope");
    return FALSE;
  }

  return TRUE;
}

static void
wyrebox_daemon_flag_keyword_update_service_finalize (GObject *object)
{
  WyreboxDaemonFlagKeywordUpdateService *self =
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_SERVICE (object);

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  G_OBJECT_CLASS (wyrebox_daemon_flag_keyword_update_service_parent_class)
      ->finalize (object);
}

static void
    wyrebox_daemon_flag_keyword_update_service_class_init
    (WyreboxDaemonFlagKeywordUpdateServiceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_flag_keyword_update_service_finalize;
}

static void
    wyrebox_daemon_flag_keyword_update_service_init
    (WyreboxDaemonFlagKeywordUpdateService * self)
{
  (void) self;
}

WyreboxDaemonFlagKeywordUpdateService
    * wyrebox_daemon_flag_keyword_update_service_new
    (WyreboxDaemonFlagKeywordUpdateServiceFunc func, gpointer user_data,
    GDestroyNotify user_data_destroy) {
  g_return_val_if_fail (func != NULL, NULL);

  WyreboxDaemonFlagKeywordUpdateService *self =
      g_object_new (WYREBOX_TYPE_DAEMON_FLAG_KEYWORD_UPDATE_SERVICE, NULL);

  self->func = func;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;

  return self;
}

gboolean
    wyrebox_daemon_flag_keyword_update_service_handle_identity
    (WyreboxDaemonFlagKeywordUpdateService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonFlagKeywordUpdateRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_FLAG_KEYWORD_UPDATE_SERVICE (self),
      FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!authorize_flag_keyword_update_identity (identity, request, error))
    return FALSE;

  if (!self->func (identity, request, &receipt, self->user_data, &local_error)) {
    if (local_error == NULL) {
      g_set_error (&local_error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "flag keyword update service failed without error detail");
    }

    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }

  if (!validate_flag_keyword_update_receipt (identity, &receipt, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_success (out_frame,
      &receipt, identity->correlation_id, error);
}
