#include "wyrebox-journal-reader.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>

#define WYREBOX_JOURNAL_SEGMENT_NAME "00000000000000000000.wbj"
#define WYREBOX_JOURNAL_RECORD_HEADER_SIZE 64
#define WYREBOX_JOURNAL_RECORD_VERSION 1
#define WYREBOX_JOURNAL_MAGIC "WYREJNL1"

struct _WyreboxJournalReader
{
  GObject parent_instance;

  char *journal_root_dir;
  char *segment_path;
  int fd;
  guint64 next_sequence;
  gsize file_size;
  guint64 offset;
};

G_DEFINE_TYPE (WyreboxJournalReader, wyrebox_journal_reader, G_TYPE_OBJECT);

static inline guint16
read_u16_le (const guint8 *buffer)
{
  return (guint16) (buffer[0] | ((guint16) buffer[1] << 8));
}

static inline guint32
read_u32_le (const guint8 *buffer)
{
  return (guint32) (buffer[0] |
      ((guint32) buffer[1] << 8) |
      ((guint32) buffer[2] << 16) | ((guint32) buffer[3] << 24));
}

static inline guint64
read_u64_le (const guint8 *buffer)
{
  return (guint64) buffer[0] |
      ((guint64) buffer[1] << 8) |
      ((guint64) buffer[2] << 16) |
      ((guint64) buffer[3] << 24) |
      ((guint64) buffer[4] << 32) |
      ((guint64) buffer[5] << 40) |
      ((guint64) buffer[6] << 48) | ((guint64) buffer[7] << 56);
}

static gboolean
read_exact (int fd, void *data, gsize size, GError **error)
{
  guint8 *cursor = data;
  gsize remaining = size;

  while (remaining > 0) {
    ssize_t read_count = read (fd, cursor, remaining);

    if (read_count < 0) {
      int saved_errno = errno;

      if (saved_errno == EINTR)
        continue;

      g_set_error (error,
          G_IO_ERROR,
          g_io_error_from_errno (saved_errno),
          "failed to read journal segment: %s", g_strerror (saved_errno));
      return FALSE;
    }

    if (read_count == 0) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "failed to read complete journal record: truncated");
      return FALSE;
    }

    cursor += (gsize) read_count;
    remaining -= (gsize) read_count;
  }

  return TRUE;
}

static gboolean
parse_event_type (const guint8 *event_type_data,
    gsize event_type_len, WyreboxJournalEventType *out_event_type)
{
  for (guint index = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
      index <= WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED; index++) {
    const gchar *canonical = wyrebox_journal_event_type_to_string (index);
    gsize canonical_len = 0;

    if (canonical == NULL)
      continue;

    canonical_len = strlen (canonical);
    if (event_type_len == canonical_len &&
        memcmp (event_type_data, canonical, event_type_len) == 0) {
      if (out_event_type != NULL)
        *out_event_type = (WyreboxJournalEventType) index;
      return TRUE;
    }
  }

  return FALSE;
}

static gsize
max_event_type_len (void)
{
  gsize max_len = 0;

  for (guint index = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
      index <= WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED; index++) {
    const gchar *canonical = wyrebox_journal_event_type_to_string (index);

    if (canonical != NULL)
      max_len = MAX (max_len, strlen (canonical));
  }

  return max_len;
}

static gboolean
event_type_length_is_canonical (guint64 declared_len)
{
  for (guint index = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
      index <= WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED; index++) {
    const gchar *canonical = wyrebox_journal_event_type_to_string (index);

    if (canonical != NULL && strlen (canonical) == declared_len)
      return TRUE;
  }

  return FALSE;
}

static gboolean
event_type_length_prefix_is_canonical (const guint8 *length_prefix,
    gsize available_len)
{
  g_return_val_if_fail (available_len <= 4, FALSE);

  for (guint index = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
      index <= WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED; index++) {
    const gchar *canonical = wyrebox_journal_event_type_to_string (index);
    gsize canonical_len = 0;
    gboolean matches = TRUE;

    if (canonical == NULL)
      continue;

    canonical_len = strlen (canonical);
    for (gsize byte_index = 0; byte_index < available_len; byte_index++) {
      guint8 expected_byte =
          (guint8) ((canonical_len >> (8 * byte_index)) & 0xff);

      if (length_prefix[byte_index] != expected_byte) {
        matches = FALSE;
        break;
      }
    }

    if (matches)
      return TRUE;
  }

  return FALSE;
}

static gboolean
event_type_prefix_is_plausible (const guint8 *event_type_data,
    gsize available_len, guint64 declared_len)
{
  if (available_len > declared_len)
    return FALSE;

  for (guint index = WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
      index <= WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED; index++) {
    const gchar *canonical = wyrebox_journal_event_type_to_string (index);
    gsize canonical_len = 0;

    if (canonical == NULL)
      continue;

    canonical_len = strlen (canonical);
    if ((guint64) canonical_len != declared_len)
      continue;

    if (available_len <= canonical_len &&
        memcmp (canonical, event_type_data, available_len) == 0)
      return TRUE;
  }

  return FALSE;
}

static gboolean
fixed_header_field_prefix_matches (const guint8 *header,
    guint64 available_size, guint64 field_offset,
    const guint8 *expected, gsize expected_len)
{
  guint64 available_in_field = 0;

  if (available_size <= field_offset)
    return TRUE;

  available_in_field = available_size - field_offset;
  if (available_in_field > expected_len)
    available_in_field = expected_len;

  return memcmp (header + field_offset, expected, (gsize) available_in_field)
      == 0;
}

static void
write_u16_le (guint8 *buffer, guint16 value)
{
  buffer[0] = (guint8) ((value >> 0) & 0xFF);
  buffer[1] = (guint8) ((value >> 8) & 0xFF);
}

static void
write_u64_le (guint8 *buffer, guint64 value)
{
  buffer[0] = (guint8) ((value >> 0) & 0xFF);
  buffer[1] = (guint8) ((value >> 8) & 0xFF);
  buffer[2] = (guint8) ((value >> 16) & 0xFF);
  buffer[3] = (guint8) ((value >> 24) & 0xFF);
  buffer[4] = (guint8) ((value >> 32) & 0xFF);
  buffer[5] = (guint8) ((value >> 40) & 0xFF);
  buffer[6] = (guint8) ((value >> 48) & 0xFF);
  buffer[7] = (guint8) ((value >> 56) & 0xFF);
}

static WyreboxJournalSafePrefixStopReason
classify_partial_header_prefix (const guint8 *header,
    guint64 available_size, guint64 expected_sequence)
{
  guint8 expected_header_size[2] = { 0 };
  guint8 expected_version[2] = { 0 };
  guint8 expected_sequence_bytes[8] = { 0 };

  if (!fixed_header_field_prefix_matches (header,
          available_size, 0,
          (const guint8 *) WYREBOX_JOURNAL_MAGIC,
          strlen (WYREBOX_JOURNAL_MAGIC)))
    return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_MAGIC;

  write_u16_le (expected_header_size, WYREBOX_JOURNAL_RECORD_HEADER_SIZE);
  if (!fixed_header_field_prefix_matches (header,
          available_size, 8, expected_header_size,
          sizeof (expected_header_size)))
    return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_HEADER_SIZE;

  write_u16_le (expected_version, WYREBOX_JOURNAL_RECORD_VERSION);
  if (!fixed_header_field_prefix_matches (header,
          available_size, 10, expected_version, sizeof (expected_version)))
    return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_VERSION;

  if (available_size > 12) {
    guint64 available_event_len_bytes = available_size - 12;

    if (available_event_len_bytes > 4)
      available_event_len_bytes = 4;

    if (!event_type_length_prefix_is_canonical (header + 12,
            (gsize) available_event_len_bytes))
      return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE;
  }

  if (available_size >= 16) {
    guint32 event_len32 = read_u32_le (header + 12);

    if (event_len32 == 0)
      return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_ZERO_EVENT_TYPE_LENGTH;

    if (event_len32 > G_MAXSIZE)
      return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SIZE;

    if (!event_type_length_is_canonical (event_len32))
      return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE;
  }

  write_u64_le (expected_sequence_bytes, expected_sequence);
  if (!fixed_header_field_prefix_matches (header,
          available_size, 16, expected_sequence_bytes,
          sizeof (expected_sequence_bytes)))
    return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SEQUENCE;

  if (available_size >= 32) {
    guint64 event_type_len64 = read_u32_le (header + 12);
    guint64 payload_len64 = read_u64_le (header + 24);

    if (payload_len64 > G_MAXSIZE)
      return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SIZE;

    if (event_type_len64 >
        G_MAXUINT64 - WYREBOX_JOURNAL_RECORD_HEADER_SIZE ||
        payload_len64 >
        G_MAXUINT64 - WYREBOX_JOURNAL_RECORD_HEADER_SIZE - event_type_len64)
      return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SIZE;
  }

  return WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_HEADER;
}

static gboolean
pread_exact (int fd, guint64 offset, void *data, gsize size, GError **error)
{
  guint8 *cursor = data;
  gsize remaining = size;

  while (remaining > 0) {
    ssize_t read_count = 0;

    if (offset > G_MAXINT64) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "journal offset %" G_GUINT64_FORMAT " is too large", offset);
      return FALSE;
    }

    read_count = pread (fd, cursor, remaining, (off_t) offset);
    if (read_count < 0) {
      int saved_errno = errno;

      if (saved_errno == EINTR)
        continue;

      g_set_error (error,
          G_IO_ERROR,
          g_io_error_from_errno (saved_errno),
          "failed to read journal segment: %s", g_strerror (saved_errno));
      return FALSE;
    }

    if (read_count == 0) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "failed to read complete journal record: truncated");
      return FALSE;
    }

    cursor += (gsize) read_count;
    offset += (guint64) read_count;
    remaining -= (gsize) read_count;
  }

  return TRUE;
}

static void
safe_prefix_set_unsafe (WyreboxJournalSafePrefix *prefix,
    WyreboxJournalSafePrefixStopReason reason,
    guint64 unsafe_offset, guint64 available_size, guint64 required_size)
{
  prefix->reached_eof = FALSE;
  prefix->unsafe_suffix_found = TRUE;
  prefix->stop_reason = reason;
  prefix->unsafe_offset = unsafe_offset;
  prefix->unsafe_available_size = available_size;
  prefix->unsafe_required_size = required_size;
}

static void
wyrebox_journal_reader_finalize (GObject *object)
{
  WyreboxJournalReader *self = WYREBOX_JOURNAL_READER (object);

  if (self->fd >= 0)
    (void) close (self->fd);

  g_clear_pointer (&self->journal_root_dir, g_free);
  g_clear_pointer (&self->segment_path, g_free);

  G_OBJECT_CLASS (wyrebox_journal_reader_parent_class)->finalize (object);
}

static void
wyrebox_journal_reader_class_init (WyreboxJournalReaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_journal_reader_finalize;
}

static void
wyrebox_journal_reader_init (WyreboxJournalReader *self)
{
  self->fd = -1;
  self->next_sequence = 1;
}

void
wyrebox_journal_record_clear (WyreboxJournalRecord *record)
{
  g_return_if_fail (record != NULL);

  g_clear_pointer (&record->payload, g_bytes_unref);
  record->offset = 0;
  record->sequence = 0;
  record->event_type = 0;
}

WyreboxJournalReader *
wyrebox_journal_reader_new (const char *journal_root_dir, GError **error)
{
  g_autoptr (WyreboxJournalReader) self = NULL;
  g_autofree char *segment_path = NULL;
  struct stat segment_stat = { 0 };
  g_autofd int fd = -1;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (journal_root_dir == NULL || *journal_root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "journal root directory is required");
    return NULL;
  }

  segment_path = g_build_filename (journal_root_dir,
      WYREBOX_JOURNAL_SEGMENT_NAME, NULL);
  fd = open (segment_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno != ENOENT) {
      int saved_errno = errno;

      g_set_error (error,
          G_IO_ERROR,
          g_io_error_from_errno (saved_errno),
          "failed to open journal segment %s: %s",
          segment_path, g_strerror (saved_errno));
      return NULL;
    }
  } else if (fstat (fd, &segment_stat) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to stat journal segment %s: %s",
        segment_path, g_strerror (saved_errno));
    return NULL;
  } else {
    if (segment_stat.st_size < 0) {
      int saved_errno = EIO;

      g_set_error (error,
          G_IO_ERROR,
          g_io_error_from_errno (saved_errno),
          "failed to read journal segment %s: %s",
          segment_path, g_strerror (saved_errno));
      return NULL;
    }

    if ((guint64) segment_stat.st_size > G_MAXSIZE) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "journal segment %s is too large", segment_path);
      return NULL;
    }
  }

  self = g_object_new (WYREBOX_TYPE_JOURNAL_READER, NULL);
  self->journal_root_dir = g_strdup (journal_root_dir);
  self->segment_path = g_steal_pointer (&segment_path);
  self->fd = fd;
  fd = -1;
  if (self->fd >= 0)
    self->file_size = (gsize) segment_stat.st_size;

  return g_steal_pointer (&self);
}

static WyreboxJournalReader *
journal_reader_new_for_segment_fd (int segment_fd,
    const char *segment_description, GError **error)
{
  g_autoptr (WyreboxJournalReader) self = NULL;
  const char *description = segment_description != NULL ?
      segment_description : "journal segment fd";
  struct stat segment_stat = { 0 };
  g_autofd int fd = -1;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (segment_fd < 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "journal segment fd is required");
    return NULL;
  }

  fd = fcntl (segment_fd, F_DUPFD_CLOEXEC, 0);
  if (fd < 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to duplicate %s: %s", description, g_strerror (saved_errno));
    return NULL;
  }

  if (fstat (fd, &segment_stat) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to stat %s: %s", description, g_strerror (saved_errno));
    return NULL;
  }

  if (segment_stat.st_size < 0) {
    int saved_errno = EIO;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to read %s: %s", description, g_strerror (saved_errno));
    return NULL;
  }

  if ((guint64) segment_stat.st_size > G_MAXSIZE) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is too large", description);
    return NULL;
  }

  self = g_object_new (WYREBOX_TYPE_JOURNAL_READER, NULL);
  self->segment_path = g_strdup (description);
  self->fd = fd;
  fd = -1;
  self->file_size = (gsize) segment_stat.st_size;

  return g_steal_pointer (&self);
}

gboolean
wyrebox_journal_reader_read_next (WyreboxJournalReader *self,
    WyreboxJournalRecord *record, gboolean *out_eof, GError **error)
{
  g_autofree guint8 *header = NULL;
  g_autofree guint8 *event_type_data = NULL;
  g_autofree guint8 *payload_data = NULL;
  g_autofree guint8 *computed_checksum = NULL;
  g_autoptr (GChecksum) checksum = NULL;
  WyreboxJournalEventType event_type = 0;
  gsize checksum_len = 0;
  gsize event_type_len = 0;
  gsize payload_len = 0;
  guint16 header_size = 0;
  guint16 version = 0;
  guint32 event_len32 = 0;
  guint64 payload_len64 = 0;
  guint64 sequence = 0;
  guint64 current_offset = 0;
  guint64 record_end_offset = 0;
  gsize remaining = 0;
  gsize remaining_after_header = 0;

  g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (self), FALSE);
  g_return_val_if_fail (record != NULL, FALSE);
  g_return_val_if_fail (out_eof != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  *out_eof = FALSE;
  wyrebox_journal_record_clear (record);
  current_offset = self->offset;

  if (self->fd < 0 || current_offset >= self->file_size) {
    *out_eof = TRUE;
    return FALSE;
  }

  if (self->file_size - current_offset < WYREBOX_JOURNAL_RECORD_HEADER_SIZE) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "failed to read complete journal record header: truncated");
    return FALSE;
  }

  header = g_malloc0 (WYREBOX_JOURNAL_RECORD_HEADER_SIZE);
  if (!read_exact (self->fd, header, WYREBOX_JOURNAL_RECORD_HEADER_SIZE, error))
    return FALSE;

  if (memcmp (header,
          WYREBOX_JOURNAL_MAGIC, strlen (WYREBOX_JOURNAL_MAGIC)) != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "invalid journal magic");
    return FALSE;
  }

  header_size = read_u16_le (header + 8);
  if (header_size != WYREBOX_JOURNAL_RECORD_HEADER_SIZE) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "invalid journal record header size: %" G_GUINT16_FORMAT, header_size);
    return FALSE;
  }

  version = read_u16_le (header + 10);
  if (version != WYREBOX_JOURNAL_RECORD_VERSION) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "invalid journal record version: %" G_GUINT16_FORMAT, version);
    return FALSE;
  }

  event_len32 = read_u32_le (header + 12);
  sequence = read_u64_le (header + 16);
  payload_len64 = read_u64_le (header + 24);

  if (sequence != self->next_sequence) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "invalid journal sequence: expected %" G_GUINT64_FORMAT
        ", found %" G_GUINT64_FORMAT, self->next_sequence, sequence);
    return FALSE;
  }

  if (payload_len64 > G_MAXSIZE || event_len32 > G_MAXSIZE) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "journal payload or event length is too large");
    return FALSE;
  }

  event_type_len = (gsize) event_len32;
  payload_len = (gsize) payload_len64;

  if (event_type_len == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "journal event type length is zero");
    return FALSE;
  }

  remaining = self->file_size - (gsize) current_offset;
  remaining_after_header = remaining - WYREBOX_JOURNAL_RECORD_HEADER_SIZE;
  if (event_type_len > remaining_after_header ||
      payload_len > remaining_after_header - event_type_len) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "journal record has invalid size");
    return FALSE;
  }

  event_type_data = g_malloc0 (event_type_len);
  if (!read_exact (self->fd, event_type_data, event_type_len, error))
    return FALSE;

  if (!parse_event_type (event_type_data, event_type_len, &event_type)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unknown journal event type");
    return FALSE;
  }

  if (payload_len > 0) {
    payload_data = g_malloc (payload_len);
    if (!read_exact (self->fd, payload_data, payload_len, error))
      return FALSE;
  }

  checksum_len = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  computed_checksum = g_malloc (checksum_len);
  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum, header, 32);
  g_checksum_update (checksum,
      (const guchar *) event_type_data, (gssize) event_type_len);
  if (payload_len > 0) {
    g_checksum_update (checksum,
        (const guchar *) payload_data, (gssize) payload_len);
  }

  g_checksum_get_digest (checksum, computed_checksum, &checksum_len);
  if (checksum_len != g_checksum_type_get_length (G_CHECKSUM_SHA256)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unsupported checksum length");
    return FALSE;
  }

  if (memcmp (header + 32, computed_checksum, checksum_len) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "journal record checksum mismatch");
    return FALSE;
  }

  record->offset = current_offset;
  record->sequence = sequence;
  record->event_type = event_type;
  if (payload_len > 0) {
    record->payload =
        g_bytes_new_take (g_steal_pointer (&payload_data), payload_len);
  } else {
    record->payload = g_bytes_new_static ("", 0);
  }

  record_end_offset =
      current_offset + WYREBOX_JOURNAL_RECORD_HEADER_SIZE +
      event_type_len + payload_len;
  self->offset = record_end_offset;
  self->next_sequence++;

  return TRUE;
}

gboolean
wyrebox_journal_reader_scan_safe_prefix (WyreboxJournalReader *self,
    WyreboxJournalSafePrefix *out_prefix, GError **error)
{
  WyreboxJournalSafePrefix prefix = { 0 };
  guint64 offset = 0;
  guint64 expected_sequence = 1;
  guint64 file_size = 0;
  gsize checksum_len = 0;
  gsize max_event_len = 0;

  g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (self), FALSE);
  g_return_val_if_fail (out_prefix != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  prefix.stop_reason = WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EOF;

  if (self->fd < 0) {
    prefix.reached_eof = TRUE;
    prefix.stop_reason = WYREBOX_JOURNAL_SAFE_PREFIX_STOP_MISSING_SEGMENT;
    *out_prefix = prefix;
    return TRUE;
  }

  if (self->file_size == 0) {
    prefix.reached_eof = TRUE;
    prefix.stop_reason = WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EMPTY_SEGMENT;
    *out_prefix = prefix;
    return TRUE;
  }

  file_size = self->file_size;
  checksum_len = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  max_event_len = max_event_type_len ();

  while (offset < file_size) {
    guint8 header[WYREBOX_JOURNAL_RECORD_HEADER_SIZE] = { 0 };
    g_autofree guint8 *event_type_data = NULL;
    g_autofree guint8 *computed_checksum = NULL;
    g_autoptr (GChecksum) checksum = NULL;
    guint64 remaining = file_size - offset;
    guint64 remaining_after_header = 0;
    guint64 event_type_len64 = 0;
    guint64 payload_len64 = 0;
    guint64 record_size = 0;
    guint64 payload_offset = 0;
    guint64 payload_remaining = 0;
    guint16 header_size = 0;
    guint16 version = 0;
    guint32 event_len32 = 0;
    guint64 sequence = 0;
    gsize digest_len = 0;

    if (remaining < WYREBOX_JOURNAL_RECORD_HEADER_SIZE) {
      WyreboxJournalSafePrefixStopReason reason =
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_HEADER;

      if (!pread_exact (self->fd, offset, header, (gsize) remaining, error))
        return FALSE;

      reason = classify_partial_header_prefix (header,
          remaining, expected_sequence);
      safe_prefix_set_unsafe (&prefix,
          reason, offset, remaining,
          reason == WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_HEADER ?
          WYREBOX_JOURNAL_RECORD_HEADER_SIZE : 0);
      *out_prefix = prefix;
      return TRUE;
    }

    if (!pread_exact (self->fd, offset, header, sizeof (header), error))
      return FALSE;

    if (memcmp (header,
            WYREBOX_JOURNAL_MAGIC, strlen (WYREBOX_JOURNAL_MAGIC)) != 0) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_MAGIC, offset, remaining, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    header_size = read_u16_le (header + 8);
    if (header_size != WYREBOX_JOURNAL_RECORD_HEADER_SIZE) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_HEADER_SIZE,
          offset, remaining, WYREBOX_JOURNAL_RECORD_HEADER_SIZE);
      *out_prefix = prefix;
      return TRUE;
    }

    version = read_u16_le (header + 10);
    if (version != WYREBOX_JOURNAL_RECORD_VERSION) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_VERSION,
          offset, remaining, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    event_len32 = read_u32_le (header + 12);
    sequence = read_u64_le (header + 16);
    payload_len64 = read_u64_le (header + 24);

    if (sequence != expected_sequence) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SEQUENCE,
          offset, remaining, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    if (payload_len64 > G_MAXSIZE || event_len32 > G_MAXSIZE) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SIZE, offset, remaining, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    event_type_len64 = event_len32;
    if (event_type_len64 == 0) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_ZERO_EVENT_TYPE_LENGTH,
          offset, remaining, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    if (event_type_len64 > max_event_len) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE,
          offset, remaining, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    if (!event_type_length_is_canonical (event_type_len64)) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE,
          offset, remaining, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    if (event_type_len64 >
        G_MAXUINT64 - WYREBOX_JOURNAL_RECORD_HEADER_SIZE ||
        payload_len64 >
        G_MAXUINT64 - WYREBOX_JOURNAL_RECORD_HEADER_SIZE - event_type_len64) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SIZE, offset, remaining, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    record_size = WYREBOX_JOURNAL_RECORD_HEADER_SIZE +
        event_type_len64 + payload_len64;
    if (record_size > remaining) {
      remaining_after_header = remaining - WYREBOX_JOURNAL_RECORD_HEADER_SIZE;
      if (remaining_after_header >= event_type_len64) {
        event_type_data = g_malloc0 ((gsize) event_type_len64);
        if (!pread_exact (self->fd,
                offset + WYREBOX_JOURNAL_RECORD_HEADER_SIZE,
                event_type_data, (gsize) event_type_len64, error))
          return FALSE;

        if (!parse_event_type (event_type_data, (gsize) event_type_len64, NULL)) {
          safe_prefix_set_unsafe (&prefix,
              WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE,
              offset, remaining, 0);
          *out_prefix = prefix;
          return TRUE;
        }
      } else if (remaining_after_header > 0) {
        event_type_data = g_malloc0 ((gsize) remaining_after_header);
        if (!pread_exact (self->fd,
                offset + WYREBOX_JOURNAL_RECORD_HEADER_SIZE,
                event_type_data, (gsize) remaining_after_header, error))
          return FALSE;

        if (!event_type_prefix_is_plausible (event_type_data,
                (gsize) remaining_after_header, event_type_len64)) {
          safe_prefix_set_unsafe (&prefix,
              WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE,
              offset, remaining, 0);
          *out_prefix = prefix;
          return TRUE;
        }
      }

      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_RECORD,
          offset, remaining, record_size);
      *out_prefix = prefix;
      return TRUE;
    }

    event_type_data = g_malloc0 ((gsize) event_type_len64);
    if (!pread_exact (self->fd,
            offset + WYREBOX_JOURNAL_RECORD_HEADER_SIZE,
            event_type_data, (gsize) event_type_len64, error))
      return FALSE;

    if (!parse_event_type (event_type_data, (gsize) event_type_len64, NULL)) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE,
          offset, record_size, 0);
      *out_prefix = prefix;
      return TRUE;
    }

    checksum = g_checksum_new (G_CHECKSUM_SHA256);
    g_checksum_update (checksum, header, 32);
    g_checksum_update (checksum,
        (const guchar *) event_type_data, (gssize) event_type_len64);

    payload_offset = offset + WYREBOX_JOURNAL_RECORD_HEADER_SIZE +
        event_type_len64;
    payload_remaining = payload_len64;
    while (payload_remaining > 0) {
      guint8 payload_buffer[8192] = { 0 };
      gsize read_size = payload_remaining > sizeof (payload_buffer) ?
          sizeof (payload_buffer) : (gsize) payload_remaining;

      if (!pread_exact (self->fd,
              payload_offset, payload_buffer, read_size, error))
        return FALSE;

      g_checksum_update (checksum, payload_buffer, (gssize) read_size);
      payload_offset += read_size;
      payload_remaining -= read_size;
    }

    digest_len = checksum_len;
    computed_checksum = g_malloc (checksum_len);
    g_checksum_get_digest (checksum, computed_checksum, &digest_len);
    if (digest_len != checksum_len) {
      g_set_error (error,
          G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unsupported checksum length");
      return FALSE;
    }

    if (memcmp (header + 32, computed_checksum, checksum_len) != 0) {
      safe_prefix_set_unsafe (&prefix,
          WYREBOX_JOURNAL_SAFE_PREFIX_STOP_CHECKSUM_MISMATCH,
          offset, record_size, record_size);
      *out_prefix = prefix;
      return TRUE;
    }

    prefix.safe_end_offset = offset + record_size;
    prefix.last_safe_sequence = sequence;
    prefix.has_last_safe_sequence = TRUE;
    offset += record_size;
    expected_sequence++;
  }

  prefix.reached_eof = TRUE;
  prefix.unsafe_suffix_found = FALSE;
  prefix.stop_reason = WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EOF;
  *out_prefix = prefix;

  return TRUE;
}

gboolean
wyrebox_journal_reader_scan_safe_prefix_for_segment_fd (int segment_fd,
    const char *segment_description, WyreboxJournalSafePrefix *out_prefix,
    GError **error)
{
  g_autoptr (WyreboxJournalReader) reader = NULL;

  g_return_val_if_fail (out_prefix != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  reader = journal_reader_new_for_segment_fd (segment_fd,
      segment_description, error);
  if (reader == NULL)
    return FALSE;

  return wyrebox_journal_reader_scan_safe_prefix (reader, out_prefix, error);
}

gboolean
wyrebox_journal_reader_seek_after_checkpoint (WyreboxJournalReader *self,
    guint64 checkpoint_offset, guint64 checkpoint_sequence, GError **error)
{
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  g_return_val_if_fail (WYREBOX_IS_JOURNAL_READER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->fd < 0 || checkpoint_offset >= self->file_size ||
      checkpoint_offset > G_MAXINT64) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "journal checkpoint offset %" G_GUINT64_FORMAT " is invalid",
        checkpoint_offset);
    return FALSE;
  }

  if (lseek (self->fd, (off_t) checkpoint_offset, SEEK_SET) < 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to seek journal segment to checkpoint offset %"
        G_GUINT64_FORMAT ": %s", checkpoint_offset, g_strerror (saved_errno));
    return FALSE;
  }

  self->offset = checkpoint_offset;
  self->next_sequence = checkpoint_sequence;

  if (!wyrebox_journal_reader_read_next (self, &record, &eof, error)) {
    if (eof) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "journal checkpoint offset %" G_GUINT64_FORMAT " is past EOF",
          checkpoint_offset);
    }
    return FALSE;
  }

  if (record.offset != checkpoint_offset ||
      record.sequence != checkpoint_sequence) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "journal checkpoint mismatch at offset %" G_GUINT64_FORMAT,
        checkpoint_offset);
    return FALSE;
  }

  return TRUE;
}
