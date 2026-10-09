#pragma once

#include "wyrebox-daemon-mailbox-list-result.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_CRITERIA 16
#define WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_TEXT_LENGTH 256

typedef enum
{
  WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SUBJECT_CONTAINS,
  WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_FROM_CONTAINS,
  WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENDER_DOMAIN,
  WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_SINCE,
  WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_BEFORE,
} WyreboxDaemonMessageSearchCriterionKind;

typedef struct
{
  WyreboxDaemonMessageSearchCriterionKind kind;
  /*
   * Search text for SUBJECT_CONTAINS, FROM_CONTAINS and SENDER_DOMAIN; NULL
   * for the date kinds.
   */
  char *text;
  /*
   * Decoded Date header bound in Unix microseconds: inclusive for SENT_SINCE,
   * exclusive for SENT_BEFORE, and unused for the text kinds.
   */
  gint64 unix_us;
} WyreboxDaemonMessageSearchCriterion;

typedef struct
{
  /*
   * Strings and @criteria are owned by the request and are released by
   * clear(). The criteria are combined with AND; an empty list matches every
   * visible message in the mailbox.
   */
  char *account_identity;
  char *mailbox_id;
  WyreboxDaemonMailboxListEntryKind namespace_kind;
  guint64 uid_validity;
  WyreboxDaemonMessageSearchCriterion *criteria;
  guint n_criteria;
} WyreboxDaemonMessageSearchRequest;

void wyrebox_daemon_message_search_request_clear (
    WyreboxDaemonMessageSearchRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMessageSearchRequest,
    wyrebox_daemon_message_search_request_clear)

/*
 * Validates and deep-copies the request fields and @criteria into @request,
 * replacing its previous contents. @criteria is borrowed and may be NULL when
 * @n_criteria is 0.
 *
 * Text criteria must be valid UTF-8, non-empty, free of control characters
 * and at most WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_TEXT_LENGTH bytes. At most
 * WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_CRITERIA criteria are accepted. Invalid
 * input fails with G_IO_ERROR_INVALID_ARGUMENT and leaves @request unchanged.
 */
gboolean wyrebox_daemon_message_search_request_init (
    WyreboxDaemonMessageSearchRequest *request,
    const char *account_identity,
    const char *mailbox_id,
    WyreboxDaemonMailboxListEntryKind namespace_kind,
    guint64 uid_validity,
    const WyreboxDaemonMessageSearchCriterion *criteria,
    guint n_criteria,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
