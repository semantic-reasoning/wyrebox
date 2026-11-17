#include "wyrebox-daemon-peer-account-authorizer.h"

#include <gio/gio.h>
#include <string.h>

struct _WyreboxDaemonPeerAccountAuthorizer
{
    GObject parent_instance;
    WyreboxDaemonConfig *config;
};

G_DEFINE_TYPE (WyreboxDaemonPeerAccountAuthorizer,
    wyrebox_daemon_peer_account_authorizer, G_TYPE_OBJECT);

static void
wyrebox_daemon_peer_account_authorizer_dispose (GObject *object)
{
    WyreboxDaemonPeerAccountAuthorizer *self =
        WYREBOX_DAEMON_PEER_ACCOUNT_AUTHORIZER (object);

    g_clear_object (&self->config);

    G_OBJECT_CLASS (wyrebox_daemon_peer_account_authorizer_parent_class)->
    dispose (object);
}

static void
wyrebox_daemon_peer_account_authorizer_class_init (
    WyreboxDaemonPeerAccountAuthorizerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = wyrebox_daemon_peer_account_authorizer_dispose;
}

static void
wyrebox_daemon_peer_account_authorizer_init (
    WyreboxDaemonPeerAccountAuthorizer *self)
{
    (void)self;
}

WyreboxDaemonPeerAccountAuthorizer *
wyrebox_daemon_peer_account_authorizer_new (WyreboxDaemonConfig *config)
{
    WyreboxDaemonPeerAccountAuthorizer *self = NULL;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_CONFIG (config), NULL);

    self = g_object_new (WYREBOX_TYPE_DAEMON_PEER_ACCOUNT_AUTHORIZER, NULL);
    self->config = g_object_ref (config);
    return self;
}

gboolean
wyrebox_daemon_peer_account_authorizer_allows (
    WyreboxDaemonPeerAccountAuthorizer *self,
    uid_t uid,
    const char *account_identity,
    GError **error)
{
    g_return_val_if_fail (WYREBOX_IS_DAEMON_PEER_ACCOUNT_AUTHORIZER (self),
        FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (account_identity == NULL || *account_identity == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "account identity is required for peer account authorization");
        return FALSE;
    }

    if (!wyrebox_daemon_config_peer_uid_allows_account (self->config, uid,
        account_identity)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "peer uid %u is not bound to account '%s'",
            (unsigned int)uid, account_identity);
        return FALSE;
    }

    return TRUE;
}
