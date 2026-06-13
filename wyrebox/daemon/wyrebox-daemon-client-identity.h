#pragma once

#include "wyrebox-daemon-request-identity.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN,
  WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI,
  WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL,
  WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER,
  WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN,
} WyreboxDaemonClientIdentityClass;

WyreboxDaemonClientIdentityClass
wyrebox_daemon_client_identity_classify_name (
    const char *caller_identity);

WyreboxDaemonClientIdentityClass
wyrebox_daemon_client_identity_classify_request (
    const WyreboxDaemonRequestIdentity *identity);

const char *wyrebox_daemon_client_identity_class_to_name (
    WyreboxDaemonClientIdentityClass identity_class);

gboolean wyrebox_daemon_client_identity_can_query_controlled_views (
    WyreboxDaemonClientIdentityClass identity_class);

G_END_DECLS
/* *INDENT-ON* */
