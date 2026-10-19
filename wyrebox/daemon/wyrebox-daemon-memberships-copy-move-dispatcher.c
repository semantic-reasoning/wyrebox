#include "wyrebox-daemon-memberships-copy-move-dispatcher.h"

#include "wyrebox-daemon-error-frame.h"

#include <gio/gio.h>

static gboolean
init_error_response_from_cause (WyreboxDaemonResponseFrame *out_frame,
    const char *request_id,
    const char *correlation_id, const GError *cause, GError **error)
{
    g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };

    if (!wyrebox_daemon_error_frame_init_from_g_error (&error_frame,
        request_id, cause, NULL, error))
        return FALSE;

    return wyrebox_daemon_response_frame_init_error (out_frame,
               &error_frame, correlation_id, error);
}

static gboolean
dispatch_copy_move (WyreboxDaemonMembershipsCopyMoveService *service,
    gboolean is_move, const char *request_id, const char *caller_identity,
    const char *account_identity, const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_autoptr (GError) local_error = NULL;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_MEMBERSHIPS_COPY_MOVE_SERVICE (
            service),
        FALSE);
    g_return_val_if_fail (request != NULL, FALSE);
    g_return_val_if_fail (out_frame != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!wyrebox_daemon_request_identity_init (&identity,
        request_id,
        caller_identity,
        account_identity, tool_identity, correlation_id, error))
        return FALSE;

    if (wyrebox_daemon_memberships_copy_move_service_handle_identity (service,
        &identity, is_move, request, out_frame, &local_error))
        return TRUE;

    return init_error_response_from_cause (out_frame,
               identity.request_id, identity.correlation_id, local_error,
               error);
}

gboolean
wyrebox_daemon_memberships_copy_dispatch
    (WyreboxDaemonMembershipsCopyMoveService *service, const char *request_id,
    const char *caller_identity, const char *account_identity,
    const char *tool_identity, const char *correlation_id,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
    return dispatch_copy_move (service, FALSE, request_id, caller_identity,
               account_identity, tool_identity, correlation_id, request,
               out_frame, error);
}

gboolean
wyrebox_daemon_memberships_move_dispatch
    (WyreboxDaemonMembershipsCopyMoveService *service, const char *request_id,
    const char *caller_identity, const char *account_identity,
    const char *tool_identity, const char *correlation_id,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
    return dispatch_copy_move (service, TRUE, request_id, caller_identity,
               account_identity, tool_identity, correlation_id, request,
               out_frame, error);
}
