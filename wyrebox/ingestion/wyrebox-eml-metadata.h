#pragma once

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * First Message-ID header value, or NULL when the header is absent.
   *
   * Duplicate Message-ID headers do not fail parsing. The first value remains
   * canonical and duplicate_message_id_count records the number of additional
   * Message-ID headers encountered.
   *
   * Ownership: caller owns and must free with g_free() or
   * wyrebox_eml_metadata_clear().
   */
  char *message_id;

  /*
   * Raw unfolded header values for selected RFC 5322 fields. RFC 2047 encoded
   * words, addresses, and dates are preserved as-is; this boundary does not
   * decode MIME headers, normalize addresses, or parse dates.
   *
   * Ownership: caller owns and must free with g_free() or
   * wyrebox_eml_metadata_clear().
   */
  char *subject;
  char *from;
  char *to;
  char *cc;
  char *bcc;
  char *date;
  char *in_reply_to;
  char *references;

  /*
   * Raw byte span for the first canonical Subject header within the RFC 5322
   * header block. The start is inclusive and the end is exclusive. When the
   * Subject header is absent, subject_span_valid is FALSE and the offsets are
   * zero.
   */
  gboolean subject_span_valid;
  guint64 subject_span_start;
  guint64 subject_span_end;

  /*
   * Size of the raw RFC 5322 message bytes supplied to the parser.
   */
  guint64 size_bytes;

  /*
   * Number of additional Message-ID headers after the first canonical value.
   */
  guint duplicate_message_id_count;
} WyreboxEmlMetadata;

/*
 * Clears owned fields in @metadata and leaves it reusable as empty metadata.
 */
void wyrebox_eml_metadata_clear (WyreboxEmlMetadata *metadata);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxEmlMetadata,
    wyrebox_eml_metadata_clear)

/*
 * @bytes: (transfer none): raw RFC 5322 message bytes.
 * @out_metadata: (out): receives parsed metadata. The caller owns fields in
 *   the result and must clear it with wyrebox_eml_metadata_clear().
 *
 * Parses only the CRLF-terminated header block before the first CRLFCRLF
 * separator. Header continuations are unfolded by replacing CRLF followed by
 * SP/HTAB with one ASCII space in the returned header value. Missing
 * Message-ID is successful and leaves message_id NULL. The only structural
 * parse failure at this boundary is a missing CRLFCRLF header/body separator.
 */
gboolean wyrebox_eml_metadata_parse_bytes (GBytes *bytes,
    WyreboxEmlMetadata *out_metadata,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
