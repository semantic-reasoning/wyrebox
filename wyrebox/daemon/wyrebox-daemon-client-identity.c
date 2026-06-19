#include "wyrebox-daemon-client-identity.h"

WyreboxDaemonClientIdentityClass
wyrebox_daemon_client_identity_classify_name (const char *caller_identity)
{
  if (g_strcmp0 (caller_identity, "admin-cli") == 0)
    return WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI;

  if (g_strcmp0 (caller_identity, "trusted-tool") == 0)
    return WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL;

  if (g_strcmp0 (caller_identity, "postfix-helper") == 0)
    return WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER;

  if (g_strcmp0 (caller_identity, "dovecot") == 0)
    return WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN;

  if (g_strcmp0 (caller_identity, "dovecot-plugin") == 0)
    return WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN;

  return WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN;
}

WyreboxDaemonClientIdentityClass
    wyrebox_daemon_client_identity_classify_request
    (const WyreboxDaemonRequestIdentity * identity)
{
  if (identity == NULL)
    return WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN;

  return wyrebox_daemon_client_identity_classify_name
      (identity->caller_identity);
}

const char *wyrebox_daemon_client_identity_class_to_name
    (WyreboxDaemonClientIdentityClass identity_class)
{
  switch (identity_class) {
    case WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI:
      return "admin-cli";
    case WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL:
      return "trusted-tool";
    case WYREBOX_DAEMON_CLIENT_IDENTITY_POSTFIX_HELPER:
      return "postfix-helper";
    case WYREBOX_DAEMON_CLIENT_IDENTITY_DOVECOT_PLUGIN:
      return "dovecot-plugin";
    case WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN:
    default:
      return "unknown";
  }
}

gboolean
    wyrebox_daemon_client_identity_can_query_controlled_views
    (WyreboxDaemonClientIdentityClass identity_class) {
  return identity_class == WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI
      || identity_class == WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL;
}

gboolean
    wyrebox_daemon_client_identity_can_mutate_facts
    (WyreboxDaemonClientIdentityClass identity_class) {
  return identity_class == WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL;
}

gboolean
    wyrebox_daemon_client_identity_can_export_datasets
    (WyreboxDaemonClientIdentityClass identity_class) {
  return identity_class == WYREBOX_DAEMON_CLIENT_IDENTITY_ADMIN_CLI
      || identity_class == WYREBOX_DAEMON_CLIENT_IDENTITY_TRUSTED_TOOL;
}
