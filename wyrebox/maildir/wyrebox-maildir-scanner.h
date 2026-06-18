#pragma once

#include <gio/gio.h>
#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX,
  WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE,
} WyreboxMaildirScanEntryKind;

typedef struct
{
  /*
   * Normalized relative mailbox path.
   *
   * The inbox is represented as the empty string.
   */
  gchar *mailbox_path;

  /*
   * Relative source path within the scanned tree.
   */
  gchar *source_path;

  WyreboxMaildirScanEntryKind kind;
} WyreboxMaildirScanEntry;

/*
 * Clears owned fields in @entry and leaves it reusable as an empty entry.
 */
void wyrebox_maildir_scan_entry_clear (WyreboxMaildirScanEntry *entry);

/*
 * Frees @entry and its owned fields.
 */
void wyrebox_maildir_scan_entry_free (WyreboxMaildirScanEntry *entry);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxMaildirScanEntry,
    wyrebox_maildir_scan_entry_clear)

#define WYREBOX_TYPE_MAILDIR_SCANNER (wyrebox_maildir_scanner_get_type ())

G_DECLARE_FINAL_TYPE (WyreboxMaildirScanner,
    wyrebox_maildir_scanner,
    WYREBOX,
    MAILDIR_SCANNER,
    GObject)

/*
 * Returns: (transfer full): a new read-only scanner.
 */
WyreboxMaildirScanner *wyrebox_maildir_scanner_new (void);

/*
 * @root_path: (type filename): Maildir tree root to scan.
 * @out_entries: (out) (transfer full): receives an ordered import plan. The
 *   caller owns the returned array and its entries.
 *
 * Produces a deterministic, read-only plan that classifies mailbox roots and
 * message files discovered under @root_path. The scanner does not mutate the
 * tree, allocate UIDs, ingest bytes, or perform duplicate resolution beyond
 * reporting each discovered entry separately.
 */
gboolean wyrebox_maildir_scanner_scan (WyreboxMaildirScanner *self,
    const gchar *root_path,
    GPtrArray **out_entries,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
