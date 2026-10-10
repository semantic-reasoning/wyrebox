#pragma once

#include "wyrebox-fact-record.h"

#include <glib-object.h>

typedef struct
{
    /*
     * Account and journal message id ("journal:<offset>:<sequence>") of the
     * delivered message the facts were extracted from.
     *
     * Ownership: owned by the payload and cleared by
     * wyrebox_facts_extracted_payload_clear().
     */
    char *account_id;
    char *message_id;

    /*
     * Extraction time as Unix microseconds. Decoded facts carry it as their
     * created_at_unix_us.
     */
    guint64 extracted_at_unix_us;

    /*
     * (element-type WyreboxFactRecord) (nullable): extracted facts in
     * extractor order. Every fact names @message_id as its first argument.
     * Decoding always sets an array, which may be empty; encoding treats NULL
     * as empty. Owned by the payload; elements are freed with
     * wyrebox_facts_extracted_payload_fact_free().
     */
    GPtrArray *facts;
} WyreboxFactsExtractedPayload;

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Frees a heap-allocated WyreboxFactRecord element of
 * WyreboxFactsExtractedPayload.facts.
 */
void wyrebox_facts_extracted_payload_fact_free (gpointer fact);

void wyrebox_facts_extracted_payload_clear (
    WyreboxFactsExtractedPayload *payload);

/*
 * Returns: (transfer full): encoded FactsExtracted journal payload bytes, or
 *   NULL with G_IO_ERROR_INVALID_ARGUMENT when @payload is incomplete or a
 *   fact does not name @payload's message as its first argument.
 */
GBytes *wyrebox_facts_extracted_payload_encode (
    const WyreboxFactsExtractedPayload *payload,
    GError **error);

/*
 * @out_payload: (out): receives decoded owned fields; clear it with
 *   wyrebox_facts_extracted_payload_clear().
 *
 * Malformed bytes fail with G_IO_ERROR_INVALID_DATA.
 */
gboolean wyrebox_facts_extracted_payload_decode (GBytes *bytes,
    WyreboxFactsExtractedPayload *out_payload,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxFactsExtractedPayload,
    wyrebox_facts_extracted_payload_clear)

G_END_DECLS
/* *INDENT-ON* */
