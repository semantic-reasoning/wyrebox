#pragma once

#include "wyrebox-local-object-store.h"

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
    WYREBOX_DELIVERY_OBJECT_CHECK_OK,
    WYREBOX_DELIVERY_OBJECT_CHECK_MISSING,
    WYREBOX_DELIVERY_OBJECT_CHECK_SIZE_MISMATCH,
    WYREBOX_DELIVERY_OBJECT_CHECK_HASH_MISMATCH,
    WYREBOX_DELIVERY_OBJECT_CHECK_INVALID_KEY,
    WYREBOX_DELIVERY_OBJECT_CHECK_UNREADABLE,
} WyreboxDeliveryObjectCheckResult;

/*
 * Reads the raw object a journaled delivery references and checks that it
 * exists, has @expected_size bytes, and matches the SHA-256 in @object_key.
 *
 * MISSING, SIZE_MISMATCH, HASH_MISMATCH and INVALID_KEY describe the stored
 * data and do not change on retry. Only a missing object file counts as
 * MISSING. UNREADABLE means the object could not be read for any other
 * reason, such as permissions, an I/O error, or a damaged directory layout,
 * and @error keeps the object store's original domain and code. INVALID_KEY
 * cannot occur for a decoded MessageDelivered payload, whose decoder
 * validates the key.
 *
 * @object_store: (transfer none): store holding the raw object.
 * @object_key: (transfer none): key of the raw object to check.
 * @error: on failure, set to a new error the caller owns.
 *
 * Returns: OK, or the failure kind with @error set to the underlying error.
 */
WyreboxDeliveryObjectCheckResult wyrebox_delivery_object_check (
    WyreboxLocalObjectStore *object_store, const char *object_key,
    guint64 expected_size, GError **error);

G_END_DECLS
