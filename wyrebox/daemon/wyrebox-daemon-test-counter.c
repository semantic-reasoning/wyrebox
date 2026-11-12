/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wyrebox-daemon-test-counter.h"

#include <glib.h>
#include <glib/gstdio.h>

void
wyrebox_daemon_test_counter_increment (const char *env_name)
{
    const char *path;
    g_autofree char *contents = NULL;
    gsize length = 0;
    guint64 count = 0;

    if (env_name == NULL || env_name[0] == '\0')
        return;

    path = g_getenv (env_name);
    if (path == NULL || path[0] == '\0')
        return;

    if (g_file_get_contents (path, &contents, &length, NULL) && length > 0)
        count = g_ascii_strtoull (contents, NULL, 10);
    contents = g_strdup_printf ("%" G_GUINT64_FORMAT, count + 1);
    g_file_set_contents (path, contents, -1, NULL);
}
