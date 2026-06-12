#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
  WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
} WyreboxDaemonMailboxListEntryKind;

typedef enum
{
  WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN,
  WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN,
  WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN,
} WyreboxDaemonMailboxListChildState;

typedef struct
{
  /*
   * All strings are owned by the entry and are released by clear().
   * For virtual entries, mailbox_id carries the stable WyreBox view_id.
   */
  WyreboxDaemonMailboxListEntryKind kind;
  char *mailbox_id;
  char *mailbox_name;
  char *hierarchy_delimiter;
  char *special_use;
  gboolean is_selectable;
  WyreboxDaemonMailboxListChildState child_state;
} WyreboxDaemonMailboxListEntry;

typedef struct
{
  GPtrArray *entries;
} WyreboxDaemonMailboxListResult;

void wyrebox_daemon_mailbox_list_entry_clear (
    WyreboxDaemonMailboxListEntry *entry);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMailboxListEntry,
    wyrebox_daemon_mailbox_list_entry_clear)

void wyrebox_daemon_mailbox_list_result_clear (
    WyreboxDaemonMailboxListResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMailboxListResult,
    wyrebox_daemon_mailbox_list_result_clear)

/*
 * Initializes @entry from borrowed string arguments and deep-copies all string
 * fields. On failure, leaves any existing contents of @entry unchanged.
 */
gboolean wyrebox_daemon_mailbox_list_entry_init (
    WyreboxDaemonMailboxListEntry *entry,
    WyreboxDaemonMailboxListEntryKind kind,
    const char *mailbox_id,
    const char *mailbox_name,
    const char *hierarchy_delimiter,
    const char *special_use,
    gboolean is_selectable,
    WyreboxDaemonMailboxListChildState child_state,
    GError **error);

/*
 * Initializes @result as an owned, initially empty LIST result.
 * Reinitialization clears any entries currently owned by @result.
 */
void wyrebox_daemon_mailbox_list_result_init_empty (
    WyreboxDaemonMailboxListResult *result);

/*
 * Appends a deep-copied entry. All string arguments are borrowed by the caller.
 */
gboolean wyrebox_daemon_mailbox_list_result_append_entry (
    WyreboxDaemonMailboxListResult *result,
    WyreboxDaemonMailboxListEntryKind kind,
    const char *mailbox_id,
    const char *mailbox_name,
    const char *hierarchy_delimiter,
    const char *special_use,
    gboolean is_selectable,
    WyreboxDaemonMailboxListChildState child_state,
    GError **error);

guint wyrebox_daemon_mailbox_list_result_get_n_entries (
    const WyreboxDaemonMailboxListResult *result);

/*
 * Returns a borrowed entry owned by @result. The pointer remains valid only
 * until @result is cleared, reinitialized, or freed.
 */
const WyreboxDaemonMailboxListEntry *
wyrebox_daemon_mailbox_list_result_get_entry (
    const WyreboxDaemonMailboxListResult *result,
    guint index);

G_END_DECLS
/* *INDENT-ON* */
