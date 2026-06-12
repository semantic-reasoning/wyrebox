#include "wyrebox-daemon-error-frame.h"

#include <gio/gio.h>

void
wyrebox_daemon_error_frame_clear (WyreboxDaemonErrorFrame *frame)
{
  if (frame == NULL)
    return;

  g_clear_pointer (&frame->request_id, g_free);
  frame->error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR;
  g_clear_pointer (&frame->message, g_free);
  g_clear_pointer (&frame->retry_hint, g_free);
}

gboolean
wyrebox_daemon_error_frame_init (WyreboxDaemonErrorFrame *frame,
    const char *request_id,
    WyreboxDaemonErrorClass error_class,
    const char *message, const char *retry_hint, GError **error)
{
  g_auto (WyreboxDaemonErrorFrame) next = { 0 };

  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (request_id == NULL || *request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "daemon error frame requires request_id");
    return FALSE;
  }

  if (message == NULL || *message == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "daemon error frame requires message");
    return FALSE;
  }

  if (wyrebox_daemon_error_class_to_string (error_class) == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "daemon error frame requires error class");
    return FALSE;
  }

  next.request_id = g_strdup (request_id);
  next.error_class = error_class;
  next.message = g_strdup (message);
  if (retry_hint != NULL && *retry_hint != '\0')
    next.retry_hint = g_strdup (retry_hint);

  wyrebox_daemon_error_frame_clear (frame);
  *frame = next;
  next.request_id = NULL;
  next.message = NULL;
  next.retry_hint = NULL;

  return TRUE;
}

gboolean
wyrebox_daemon_error_frame_init_from_g_error (WyreboxDaemonErrorFrame *frame,
    const char *request_id,
    const GError *cause, const char *retry_hint, GError **error)
{
  WyreboxDaemonErrorClass error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR;

  g_return_val_if_fail (cause != NULL, FALSE);

  if (cause->domain == G_IO_ERROR)
    error_class = wyrebox_daemon_error_class_from_g_error_code (cause->code);

  return wyrebox_daemon_error_frame_init (frame,
      request_id, error_class, cause->message, retry_hint, error);
}
