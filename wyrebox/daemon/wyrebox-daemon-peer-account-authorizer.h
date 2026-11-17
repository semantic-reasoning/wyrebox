#pragma once

#include "wyrebox-daemon-config.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_PEER_ACCOUNT_AUTHORIZER \
    (wyrebox_daemon_peer_account_authorizer_get_type ())
G_DECLARE_FINAL_TYPE (WyreboxDaemonPeerAccountAuthorizer,
    wyrebox_daemon_peer_account_authorizer, WYREBOX,
    DAEMON_PEER_ACCOUNT_AUTHORIZER, GObject)

/*
 * Returns: (transfer full): authorizer built from @config peer bindings.
 * @config is borrowed for the lifetime of the authorizer.
 */
WyreboxDaemonPeerAccountAuthorizer *
wyrebox_daemon_peer_account_authorizer_new (WyreboxDaemonConfig *config);

/*
 * Returns: whether peer @uid may claim @account_identity. Failures use
 * G_IO_ERROR_PERMISSION_DENIED when the peer is unbound or the account is
 * outside the allowlist; G_IO_ERROR_INVALID_ARGUMENT for bad inputs.
 */
gboolean wyrebox_daemon_peer_account_authorizer_allows (
    WyreboxDaemonPeerAccountAuthorizer *self,
    uid_t uid,
    const char *account_identity,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
