#include "wyrebox-journal-writer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>

#define WYREBOX_JOURNAL_SEGMENT_NAME "00000000000000000000.wbj"
#define WYREBOX_JOURNAL_RECORD_HEADER_SIZE 64
#define WYREBOX_JOURNAL_RECORD_VERSION 1

struct _WyreboxJournalWriter
{
  GObject parent_instance;

  char *journal_root_dir;
  char *segment_path;
  int fd;
  guint64 next_sequence;
  gboolean failed;
};

G_DEFINE_TYPE (WyreboxJournalWriter, wyrebox_journal_writer, G_TYPE_OBJECT);

static const char *event_type_names[] = {
  "MessageDelivered",
  "FlagChanged",
  "KeywordChanged",
  "FactInserted",
  "FactRetracted",
  "DerivedViewMembershipChanged",
};

static inline void
write_u16_le (guint8 *dst, guint16 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xFF);
  dst[1] = (guint8) ((value >> 8) & 0xFF);
}

static inline void
write_u32_le (guint8 *dst, guint32 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xFF);
  dst[1] = (guint8) ((value >> 8) & 0xFF);
  dst[2] = (guint8) ((value >> 16) & 0xFF);
  dst[3] = (guint8) ((value >> 24) & 0xFF);
}

static inline void
write_u64_le (guint8 *dst, guint64 value)
{
  dst[0] = (guint8) ((value >> 0) & 0xFF);
  dst[1] = (guint8) ((value >> 8) & 0xFF);
  dst[2] = (guint8) ((value >> 16) & 0xFF);
  dst[3] = (guint8) ((value >> 24) & 0xFF);
  dst[4] = (guint8) ((value >> 32) & 0xFF);
  dst[5] = (guint8) ((value >> 40) & 0xFF);
  dst[6] = (guint8) ((value >> 48) & 0xFF);
  dst[7] = (guint8) ((value >> 56) & 0xFF);
}

static gboolean
validate_event_type (WyreboxJournalEventType event_type)
{
  return event_type >= WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED &&
      event_type <= WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED;
}

const char *
wyrebox_journal_event_type_to_string (WyreboxJournalEventType event_type)
{
  if (!validate_event_type (event_type))
    return NULL;

  return event_type_names[event_type];
}

static gboolean
write_all (int fd, const guint8 *data, gsize size, GError **error)
{
  while (size > 0) {
    ssize_t wrote = write (fd, data, size);

    if (wrote < 0) {
      int saved_errno = errno;

      if (saved_errno == EINTR)
        continue;

      g_set_error (error,
          G_IO_ERROR,
          g_io_error_from_errno (saved_errno),
          "failed to write journal record: %s", g_strerror (saved_errno));
      return FALSE;
    }

    data += wrote;
    size -= (gsize) wrote;
  }

  return TRUE;
}

static gboolean
fsync_directory_path (const char *path, GError **error)
{
  g_autofd int fd = -1;

  fd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to open directory %s for fsync: %s",
        path, g_strerror (saved_errno));
    return FALSE;
  }

  if (fsync (fd) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to fsync directory %s: %s", path, g_strerror (saved_errno));
    return FALSE;
  }

  return TRUE;
}

static gboolean
fsync_journal_root_setup (const char *journal_root_dir, GError **error)
{
  g_autofree char *journal_parent_dir = g_path_get_dirname (journal_root_dir);

  if (!fsync_directory_path (journal_parent_dir, error))
    return FALSE;

  if (!fsync_directory_path (journal_root_dir, error))
    return FALSE;

  return TRUE;
}

static void
wyrebox_journal_writer_finalize (GObject *object)
{
  WyreboxJournalWriter *self = WYREBOX_JOURNAL_WRITER (object);

  if (self->fd >= 0)
    (void) close (self->fd);

  g_clear_pointer (&self->journal_root_dir, g_free);
  g_clear_pointer (&self->segment_path, g_free);

  G_OBJECT_CLASS (wyrebox_journal_writer_parent_class)->finalize (object);
}

static void
wyrebox_journal_writer_class_init (WyreboxJournalWriterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_journal_writer_finalize;
}

static void
wyrebox_journal_writer_init (WyreboxJournalWriter *self)
{
  self->fd = -1;
  self->next_sequence = 1;
}

WyreboxJournalWriter *
wyrebox_journal_writer_new (const char *journal_root_dir, GError **error)
{
  g_autoptr (WyreboxJournalWriter) self = NULL;
  g_autofree char *segment_path = NULL;
  struct stat segment_stat = { 0 };

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (journal_root_dir == NULL || *journal_root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "journal root directory is required");
    return NULL;
  }

  if (g_mkdir_with_parents (journal_root_dir, 0700) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to create journal root %s: %s",
        journal_root_dir, g_strerror (saved_errno));
    return NULL;
  }

  if (!fsync_journal_root_setup (journal_root_dir, error))
    return NULL;

  segment_path = g_build_filename (journal_root_dir,
      WYREBOX_JOURNAL_SEGMENT_NAME, NULL);

  int fd = open (segment_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to open journal segment %s: %s",
        segment_path, g_strerror (saved_errno));
    return NULL;
  }

  if (fstat (fd, &segment_stat) != 0) {
    int saved_errno = errno;

    (void) close (fd);
    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to stat journal segment %s: %s",
        segment_path, g_strerror (saved_errno));
    return NULL;
  }

  if (segment_stat.st_size != 0) {
    (void) close (fd);
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "journal does not support rotated or non-empty segments");
    return NULL;
  }

  if (!fsync_directory_path (journal_root_dir, error)) {
    (void) close (fd);
    return NULL;
  }

  self = g_object_new (WYREBOX_TYPE_JOURNAL_WRITER, NULL);
  self->journal_root_dir = g_strdup (journal_root_dir);
  self->segment_path = g_steal_pointer (&segment_path);
  self->fd = fd;

  return g_steal_pointer (&self);
}

gboolean
wyrebox_journal_writer_append (WyreboxJournalWriter *self,
    WyreboxJournalEventType event_type,
    GBytes *payload, guint64 *out_offset, guint64 *out_sequence, GError **error)
{
  g_autofree guint8 *header = NULL;
  g_autofree guint8 *checksum_buffer = NULL;
  const guint8 *payload_data = NULL;
  g_autoptr (GChecksum) checksum = NULL;
  g_autofree char *event_type_name = NULL;
  gsize payload_size = 0;
  gsize checksum_len = 0;
  guint16 header_size = WYREBOX_JOURNAL_RECORD_HEADER_SIZE;
  guint64 sequence = 0;
  off_t offset = 0;

  g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (self), FALSE);
  g_return_val_if_fail (out_offset != NULL, FALSE);
  g_return_val_if_fail (out_sequence != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->failed) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "journal writer is in failed state");
    return FALSE;
  }

  if (!validate_event_type (event_type)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "invalid journal event type: %d", (int) event_type);
    return FALSE;
  }

  event_type_name =
      g_strdup (wyrebox_journal_event_type_to_string (event_type));
  if (payload != NULL)
    payload_data = g_bytes_get_data (payload, &payload_size);

  sequence = self->next_sequence;

  *out_offset = 0;
  *out_sequence = 0;

  offset = lseek (self->fd, 0, SEEK_END);
  if (offset < 0) {
    int saved_errno = errno;

    self->failed = TRUE;
    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to seek journal end: %s", g_strerror (saved_errno));
    return FALSE;
  }

  header = g_malloc0 (WYREBOX_JOURNAL_RECORD_HEADER_SIZE);
  memcpy (header, "WYREJNL1", strlen ("WYREJNL1") + 1);
  write_u16_le (header + 8, header_size);
  write_u16_le (header + 10, WYREBOX_JOURNAL_RECORD_VERSION);
  write_u32_le (header + 12, (guint32) strlen (event_type_name));
  write_u64_le (header + 16, sequence);
  write_u64_le (header + 24, (guint64) payload_size);

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum, header, 32);
  g_checksum_update (checksum,
      (const guchar *) event_type_name, (gssize) strlen (event_type_name));
  g_checksum_update (checksum, payload_data, (gssize) payload_size);

  checksum_len = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  checksum_buffer = g_malloc (checksum_len);
  g_checksum_get_digest (checksum, checksum_buffer, &checksum_len);

  memcpy (header + 32, checksum_buffer, checksum_len);

  if (!write_all (self->fd, header, WYREBOX_JOURNAL_RECORD_HEADER_SIZE, error)) {
    self->failed = TRUE;
    return FALSE;
  }

  if (!write_all (self->fd,
          (const guint8 *) event_type_name, strlen (event_type_name), error)) {
    self->failed = TRUE;
    return FALSE;
  }

  if (payload_size > 0) {
    if (!write_all (self->fd, payload_data, payload_size, error)) {
      self->failed = TRUE;
      return FALSE;
    }
  }

  if (fsync (self->fd) != 0) {
    int saved_errno = errno;

    self->failed = TRUE;
    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to fsync journal segment %s: %s",
        self->segment_path, g_strerror (saved_errno));
    return FALSE;
  }

  *out_offset = (guint64) offset;
  *out_sequence = sequence;
  self->next_sequence = sequence + 1;

  return TRUE;
}
