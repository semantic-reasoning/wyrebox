#include "wyrebox-daemon-memberships-copy-move-service.h"

#include <gio/gio.h>

struct _WyreboxDaemonMembershipsCopyMoveService
{
    GObject parent_instance;

    WyreboxDaemonMembershipsCopyMoveServiceFunc func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
};

G_DEFINE_TYPE (WyreboxDaemonMembershipsCopyMoveService,
    wyrebox_daemon_memberships_copy_move_service, G_TYPE_OBJECT);

static gboolean
caller_can_copy_move (const char *caller_identity)
{
    return g_strcmp0 (caller_identity, "dovecot") == 0;
}

static gboolean
authorize_memberships_copy_move_identity (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonMembershipsCopyMoveRequest *request,
    GError **error)
{
    if (!caller_can_copy_move (identity->caller_identity)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "caller is not authorized to copy or move mailbox memberships");
        return FALSE;
    }

    if (identity->account_identity == NULL
        || g_strcmp0 (identity->account_identity,
        request->account_identity) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "caller is not authorized for memberships copy/move account "
            "scope");
        return FALSE;
    }

    return TRUE;
}

static gboolean
validate_memberships_copy_move_receipt (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonSuccessReceipt *receipt, GError **error)
{
    if (receipt->request_id == NULL ||
        g_strcmp0 (receipt->request_id, identity->request_id) != 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships copy/move receipt request_id must match request "
            "envelope");
        return FALSE;
    }

    return TRUE;
}

static void
wyrebox_daemon_memberships_copy_move_service_finalize (GObject *object)
{
    WyreboxDaemonMembershipsCopyMoveService *self =
        WYREBOX_DAEMON_MEMBERSHIPS_COPY_MOVE_SERVICE (object);

    if (self->user_data_destroy != NULL && self->user_data != NULL)
        self->user_data_destroy (self->user_data);

    G_OBJECT_CLASS (wyrebox_daemon_memberships_copy_move_service_parent_class)
    ->finalize (object);
}

static void
wyrebox_daemon_memberships_copy_move_service_class_init
    (WyreboxDaemonMembershipsCopyMoveServiceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize =
        wyrebox_daemon_memberships_copy_move_service_finalize;
}

static void
wyrebox_daemon_memberships_copy_move_service_init
    (WyreboxDaemonMembershipsCopyMoveService *self)
{
    (void)self;
}

WyreboxDaemonMembershipsCopyMoveService *
wyrebox_daemon_memberships_copy_move_service_new
    (WyreboxDaemonMembershipsCopyMoveServiceFunc func, gpointer user_data,
    GDestroyNotify user_data_destroy)
{
    g_return_val_if_fail (func != NULL, NULL);

    WyreboxDaemonMembershipsCopyMoveService *self = g_object_new
            (WYREBOX_TYPE_DAEMON_MEMBERSHIPS_COPY_MOVE_SERVICE, NULL);

    self->func = func;
    self->user_data = user_data;
    self->user_data_destroy = user_data_destroy;

    return self;
}

gboolean
wyrebox_daemon_memberships_copy_move_service_handle_identity
    (WyreboxDaemonMembershipsCopyMoveService *self,
    const WyreboxDaemonRequestIdentity *identity, gboolean is_move,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
    g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };
    g_autoptr (GError) local_error = NULL;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_MEMBERSHIPS_COPY_MOVE_SERVICE (
            self),
        FALSE);
    g_return_val_if_fail (identity != NULL, FALSE);
    g_return_val_if_fail (request != NULL, FALSE);
    g_return_val_if_fail (out_frame != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!authorize_memberships_copy_move_identity (identity, request, error))
        return FALSE;

    if (!self->func (identity, is_move, request, &receipt, self->user_data,
        &local_error)) {
        if (local_error == NULL) {
            g_set_error (&local_error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "memberships copy/move service failed without error detail");
        }

        g_propagate_error (error, g_steal_pointer (&local_error));
        return FALSE;
    }

    if (!validate_memberships_copy_move_receipt (identity, &receipt, error))
        return FALSE;

    return wyrebox_daemon_response_frame_init_success (out_frame,
               &receipt, identity->correlation_id, error);
}
