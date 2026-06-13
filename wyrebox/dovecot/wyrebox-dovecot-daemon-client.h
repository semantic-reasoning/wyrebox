#pragma once

#include "wyrebox-daemon-mailbox-select-result.h"

#include <gio/gio.h>
#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_dovecot_daemon_client_select_mailbox (
    const char *socket_path,
    const char *account_identity,
    const char *mailbox_name,
    WyreboxDaemonMailboxSelectResult *out_result,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
