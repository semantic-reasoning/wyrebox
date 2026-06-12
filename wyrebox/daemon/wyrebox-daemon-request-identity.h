#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Required daemon request identifier used to correlate request, response,
   * logs, and retry inspection.
   */
  char *request_id;

  /*
   * Optional caller, account, tool, and operation correlation identities.
   */
  char *caller_identity;
  char *account_identity;
  char *tool_identity;
  char *correlation_id;
} WyreboxDaemonRequestIdentity;

/*
 * Clears owned fields in @identity and leaves it reusable as an empty identity.
 */
void wyrebox_daemon_request_identity_clear (
    WyreboxDaemonRequestIdentity *identity);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonRequestIdentity,
    wyrebox_daemon_request_identity_clear)

/*
 * Initializes @identity from request-envelope fields.
 *
 * @request_id is required. Other strings are optional and copied when non-NULL
 * and non-empty. On success, any previous contents of @identity are cleared and
 * replaced. On failure, @identity is left unchanged.
 */
gboolean wyrebox_daemon_request_identity_init (
    WyreboxDaemonRequestIdentity *identity,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
