#pragma once

#include <glib-object.h>

G_BEGIN_DECLS
#define WYREBOX_TYPE_LOCAL_OBJECT_STORE (wyrebox_local_object_store_get_type())
G_DECLARE_FINAL_TYPE (WyreboxLocalObjectStore,
    wyrebox_local_object_store, WYREBOX, LOCAL_OBJECT_STORE, GObject)
     typedef enum
     {
       WYREBOX_LOCAL_OBJECT_STORE_ERROR_HASH_MISMATCH,
     } WyreboxLocalObjectStoreError;

#define WYREBOX_LOCAL_OBJECT_STORE_ERROR \
  (wyrebox_local_object_store_error_quark ())

     GQuark wyrebox_local_object_store_error_quark (void);

/*
 * Returns: (transfer full): a new local object store rooted at @root_dir.
 */
     WyreboxLocalObjectStore *wyrebox_local_object_store_new (const char
    *root_dir, GError **error);

/*
 * Opens an already-initialized local object store without creating directories.
 *
 * Returns: (transfer full): a new local object store rooted at @root_dir.
 */
     WyreboxLocalObjectStore *wyrebox_local_object_store_open_existing (const
    char *root_dir, GError **error);

/*
 * @bytes: (transfer none): immutable bytes to store.
 * @out_object_key: (out) (transfer full): receives the deterministic object key.
 */
     gboolean wyrebox_local_object_store_put_bytes (WyreboxLocalObjectStore
    *self, GBytes *bytes, char **out_object_key, GError **error);

/*
 * Returns: (transfer full): stored object bytes for @object_key.
 */
     GBytes *wyrebox_local_object_store_get_bytes (WyreboxLocalObjectStore
    *self, const char *object_key, GError **error);

G_END_DECLS
