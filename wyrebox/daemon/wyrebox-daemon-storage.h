#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define WYREBOX_DAEMON_STORAGE_JOURNAL_MARKER "wyrebox-journal.marker"
#define WYREBOX_DAEMON_STORAGE_OBJECT_STORE_MARKER "wyrebox-object-store.marker"

/*
 * Storage markers prove that the journal root and the object store root are
 * the volumes this installation initialized, so an unmounted mount point is
 * never mistaken for a new installation. Each root holds one marker with the
 * same UUIDv7 storage ID. Only wyreboxd writes them, through
 * wyrebox_daemon_storage_initialize().
 *
 * Errors are in the G_IO_ERROR domain:
 * - G_IO_ERROR_NOT_FOUND: a marker is missing (storage not initialized or a
 *   volume not mounted), or initialization refused because only one root
 *   holds data.
 * - G_IO_ERROR_INVALID_DATA: a marker is malformed, has the wrong role or an
 *   invalid storage ID, or the two storage IDs differ.
 * - G_IO_ERROR_NOT_SUPPORTED: a marker has an unknown format version.
 * - other codes: a marker or root could not be read or written.
 */

/*
 * Checks that both roots carry valid markers with the same storage ID. Never
 * creates or modifies anything.
 *
 * @out_storage_id: (out) (optional) (transfer full): on success, the
 *   canonical lowercase storage ID; free with g_free().
 */
gboolean wyrebox_daemon_storage_check_initialized (
    const char *journal_root_dir,
    const char *object_root_dir,
    char **out_storage_id,
    GError **error);

/*
 * Initializes storage once, or adopts existing unmarked storage. Writes the
 * missing markers only when both roots hold data or neither does: the journal
 * holds data when its segment file is not empty, and the object store when
 * objects/sha256 has an entry. A lone existing marker supplies the storage ID;
 * otherwise a new UUIDv7 is generated. Refuses without writing anything when
 * only one root holds data or when an existing marker is invalid or does not
 * match. Creates the journal root and objects/sha256 before writing markers.
 *
 * @out_already_initialized: (out) (optional): TRUE when both markers already
 *   existed and matched, so nothing was written.
 * @out_storage_id: (out) (optional) (transfer full): on success, the storage
 *   ID; free with g_free().
 */
gboolean wyrebox_daemon_storage_initialize (
    const char *journal_root_dir,
    const char *object_root_dir,
    gboolean *out_already_initialized,
    char **out_storage_id,
    GError **error);

G_END_DECLS
