#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  char *mailbox_id;
  char *mailbox_name;
  char *special_use;
  gboolean is_selectable;
  gboolean is_virtual;
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

gboolean wyrebox_daemon_mailbox_list_entry_init (
    WyreboxDaemonMailboxListEntry *entry,
    const char *mailbox_id,
    const char *mailbox_name,
    const char *special_use,
    gboolean is_selectable,
    gboolean is_virtual,
    GError **error);

void wyrebox_daemon_mailbox_list_result_init_empty (
    WyreboxDaemonMailboxListResult *result);

gboolean wyrebox_daemon_mailbox_list_result_append_entry (
    WyreboxDaemonMailboxListResult *result,
    const char *mailbox_id,
    const char *mailbox_name,
    const char *special_use,
    gboolean is_selectable,
    gboolean is_virtual,
    GError **error);

guint wyrebox_daemon_mailbox_list_result_get_n_entries (
    const WyreboxDaemonMailboxListResult *result);

const WyreboxDaemonMailboxListEntry *
wyrebox_daemon_mailbox_list_result_get_entry (
    const WyreboxDaemonMailboxListResult *result,
    guint index);

G_END_DECLS
/* *INDENT-ON* */
