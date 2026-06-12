#include "wyrebox-daemon-mailbox-list-result.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
validate_required_text (const char *value,
    const char *field_name, GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "mailbox LIST %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "mailbox LIST %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_optional_text (const char *value,
    const char *field_name, GError **error)
{
  if (value == NULL || *value == '\0')
    return TRUE;

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "mailbox LIST %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static void
free_entry_pointer (gpointer data)
{
  WyreboxDaemonMailboxListEntry *entry = data;

  wyrebox_daemon_mailbox_list_entry_clear (entry);
  g_free (entry);
}

void
wyrebox_daemon_mailbox_list_entry_clear (WyreboxDaemonMailboxListEntry *entry)
{
  if (entry == NULL)
    return;

  g_clear_pointer (&entry->mailbox_id, g_free);
  g_clear_pointer (&entry->mailbox_name, g_free);
  g_clear_pointer (&entry->special_use, g_free);
  entry->is_selectable = FALSE;
  entry->is_virtual = FALSE;
}

gboolean
wyrebox_daemon_mailbox_list_entry_init (WyreboxDaemonMailboxListEntry *entry,
    const char *mailbox_id,
    const char *mailbox_name,
    const char *special_use,
    gboolean is_selectable, gboolean is_virtual, GError **error)
{
  g_auto (WyreboxDaemonMailboxListEntry) next = { 0 };

  g_return_val_if_fail (entry != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_text (mailbox_id, "mailbox_id", error))
    return FALSE;

  if (!validate_required_text (mailbox_name, "mailbox_name", error))
    return FALSE;

  if (!validate_optional_text (special_use, "special_use", error))
    return FALSE;

  next.mailbox_id = g_strdup (mailbox_id);
  next.mailbox_name = g_strdup (mailbox_name);
  if (special_use != NULL && *special_use != '\0')
    next.special_use = g_strdup (special_use);
  next.is_selectable = is_selectable;
  next.is_virtual = is_virtual;

  wyrebox_daemon_mailbox_list_entry_clear (entry);
  *entry = next;
  next.mailbox_id = NULL;
  next.mailbox_name = NULL;
  next.special_use = NULL;

  return TRUE;
}

void
wyrebox_daemon_mailbox_list_result_clear (WyreboxDaemonMailboxListResult
    *result)
{
  if (result == NULL)
    return;

  g_clear_pointer (&result->entries, g_ptr_array_unref);
}

void
wyrebox_daemon_mailbox_list_result_init_empty (WyreboxDaemonMailboxListResult
    *result)
{
  g_return_if_fail (result != NULL);

  wyrebox_daemon_mailbox_list_result_clear (result);
  result->entries = g_ptr_array_new_with_free_func (free_entry_pointer);
}

gboolean
wyrebox_daemon_mailbox_list_result_append_entry (WyreboxDaemonMailboxListResult
    *result, const char *mailbox_id, const char *mailbox_name,
    const char *special_use, gboolean is_selectable, gboolean is_virtual,
    GError **error)
{
  g_auto (WyreboxDaemonMailboxListEntry) next = { 0 };
  WyreboxDaemonMailboxListEntry *entry = NULL;

  g_return_val_if_fail (result != NULL, FALSE);
  g_return_val_if_fail (result->entries != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_daemon_mailbox_list_entry_init (&next,
          mailbox_id, mailbox_name, special_use, is_selectable, is_virtual,
          error))
    return FALSE;

  entry = g_new0 (WyreboxDaemonMailboxListEntry, 1);
  *entry = next;
  next.mailbox_id = NULL;
  next.mailbox_name = NULL;
  next.special_use = NULL;

  g_ptr_array_add (result->entries, entry);
  return TRUE;
}

guint
wyrebox_daemon_mailbox_list_result_get_n_entries (const
    WyreboxDaemonMailboxListResult *result)
{
  if (result == NULL || result->entries == NULL)
    return 0;

  return result->entries->len;
}

const WyreboxDaemonMailboxListEntry *
wyrebox_daemon_mailbox_list_result_get_entry (const
    WyreboxDaemonMailboxListResult *result, guint index)
{
  g_return_val_if_fail (result != NULL, NULL);
  g_return_val_if_fail (result->entries != NULL, NULL);
  g_return_val_if_fail (index < result->entries->len, NULL);

  return g_ptr_array_index (result->entries, index);
}
