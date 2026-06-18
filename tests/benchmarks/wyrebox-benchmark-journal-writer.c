#include "wyrebox-journal-writer.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *name = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, name, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static void
run_microbenchmark (void)
{
  static const guint8 payload_data[] = "journal-writer-benchmark";
  g_autofree char *root = NULL;
  gint64 elapsed_us = 0;

  root = g_dir_make_tmp ("wyrebox-benchmark-journal-writer-XXXXXX", NULL);
  g_assert_nonnull (root);

  {
    g_autofree char *journal_root = g_build_filename (root, "journal", NULL);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (GBytes) payload = NULL;
    guint64 offset = 0;
    guint64 sequence = 0;
    gint64 start_us = 0;
    gint64 end_us = 0;

    writer = wyrebox_journal_writer_new (journal_root, &error);
    g_assert_no_error (error);
    g_assert_nonnull (writer);

    payload = g_bytes_new_static (payload_data, sizeof (payload_data) - 1);

    start_us = g_get_monotonic_time ();
    g_assert_true (wyrebox_journal_writer_append (writer,
            WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED, payload,
            &offset, &sequence, &error));
    end_us = g_get_monotonic_time ();
    g_assert_no_error (error);
    g_assert_cmpuint (sequence, >, 0);

    elapsed_us = end_us - start_us;

    g_print ("{\"schema\":\"wyrebox-benchmark-report/v1\",");
    g_print ("\"suite\":\"journal\",");
    g_print ("\"case\":\"journal-writer-append\",");
    g_print ("\"metric\":\"elapsed_us\",");
    g_print ("\"value\":%" G_GINT64_FORMAT ",", elapsed_us);
    g_print ("\"status\":\"ok\"}\n");
  }

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  run_microbenchmark ();

  return 0;
}
