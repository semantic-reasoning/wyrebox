#include "wyrebox-delivery-object-check.h"

#include <gio/gio.h>

WyreboxDeliveryObjectCheckResult
wyrebox_delivery_object_check (WyreboxLocalObjectStore *object_store,
    const char *object_key, guint64 expected_size, GError **error)
{
    g_autoptr (GBytes) object_bytes = NULL;
    g_autoptr (GError) local_error = NULL;
    gsize object_size = 0;

    g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store),
        WYREBOX_DELIVERY_OBJECT_CHECK_UNREADABLE);
    g_return_val_if_fail (error == NULL || *error == NULL,
        WYREBOX_DELIVERY_OBJECT_CHECK_UNREADABLE);

    object_bytes = wyrebox_local_object_store_get_bytes (object_store,
            object_key, &local_error);
    if (object_bytes == NULL) {
        WyreboxDeliveryObjectCheckResult result =
            WYREBOX_DELIVERY_OBJECT_CHECK_UNREADABLE;

        if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT) ||
            g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            result = WYREBOX_DELIVERY_OBJECT_CHECK_MISSING;
        else if (g_error_matches (local_error,
            WYREBOX_LOCAL_OBJECT_STORE_ERROR,
            WYREBOX_LOCAL_OBJECT_STORE_ERROR_HASH_MISMATCH))
            result = WYREBOX_DELIVERY_OBJECT_CHECK_HASH_MISMATCH;
        else if (g_error_matches (local_error, G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT))
            result = WYREBOX_DELIVERY_OBJECT_CHECK_INVALID_KEY;

        g_propagate_error (error, g_steal_pointer (&local_error));
        return result;
    }

    (void)g_bytes_get_data (object_bytes, &object_size);
    if (object_size != expected_size) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "raw object %s has mismatched size: expected %" G_GUINT64_FORMAT
            ", got %" G_GSIZE_FORMAT, object_key, expected_size, object_size);
        return WYREBOX_DELIVERY_OBJECT_CHECK_SIZE_MISMATCH;
    }

    return WYREBOX_DELIVERY_OBJECT_CHECK_OK;
}
