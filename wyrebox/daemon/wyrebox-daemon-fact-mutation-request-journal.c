#include "wyrebox-daemon-fact-mutation-request.h"

#include <gio/gio.h>

gboolean
wyrebox_daemon_fact_mutation_request_append_journal (const
    WyreboxDaemonFactMutationRequest *request,
    WyreboxJournalWriter *journal_writer,
    guint64 *out_offset, guint64 *out_sequence, GError **error)
{
    WyreboxJournalEventType event_type =
        WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED;
    g_autoptr (GBytes) payload = NULL;

    g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!wyrebox_daemon_fact_mutation_request_get_event (request,
        &event_type, error))
        return FALSE;

    payload = wyrebox_daemon_fact_mutation_request_encode (request, error);
    if (payload == NULL)
        return FALSE;

    return wyrebox_journal_writer_append (journal_writer, event_type, payload,
               out_offset, out_sequence, error);
}
