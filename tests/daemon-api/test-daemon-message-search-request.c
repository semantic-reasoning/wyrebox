#include "wyrebox-daemon-message-search-request.h"

#include <gio/gio.h>
#include <string.h>

static const WyreboxDaemonMessageSearchCriterion subject_criteria[] = {
    {WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SUBJECT_CONTAINS, (char *)"Größe",
     0},
    {WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_SINCE, NULL, 1000},
};

static gboolean
init_ordinary (WyreboxDaemonMessageSearchRequest *request,
    const WyreboxDaemonMessageSearchCriterion *criteria, guint n_criteria,
    GError **error)
{
    return wyrebox_daemon_message_search_request_init (request, "account-1",
               "mailbox-inbox", WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77,
               criteria, n_criteria, error);
}

static void
assert_rejects_criterion (const WyreboxDaemonMessageSearchCriterion
    *criterion)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

    g_assert_false (init_ordinary (&request, criterion, 1, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null (request.criteria);
    g_assert_cmpuint (request.n_criteria, ==, 0);
}

static void
test_message_search_request_copies_fields (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_message_search_request_init (&request,
        "account-1", "view-important",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL, 77, subject_criteria,
        G_N_ELEMENTS (subject_criteria), &error));
    g_assert_no_error (error);

    g_assert_cmpstr (request.account_identity, ==, "account-1");
    g_assert_cmpstr (request.mailbox_id, ==, "view-important");
    g_assert_cmpint (request.namespace_kind, ==,
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
    g_assert_cmpuint (request.uid_validity, ==, 77);
    g_assert_cmpuint (request.n_criteria, ==, 2);
    g_assert_cmpint (request.criteria[0].kind, ==,
        WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SUBJECT_CONTAINS);
    g_assert_cmpstr (request.criteria[0].text, ==, "Größe");
    g_assert_true (request.criteria[0].text != subject_criteria[0].text);
    g_assert_cmpint (request.criteria[1].kind, ==,
        WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_SINCE);
    g_assert_null (request.criteria[1].text);
    g_assert_cmpint (request.criteria[1].unix_us, ==, 1000);
}

static void
test_message_search_request_accepts_no_criteria (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

    g_assert_true (init_ordinary (&request, NULL, 0, &error));
    g_assert_no_error (error);
    g_assert_null (request.criteria);
    g_assert_cmpuint (request.n_criteria, ==, 0);
}

static void
test_message_search_request_reinitializes (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
    const WyreboxDaemonMessageSearchCriterion domain[] = {
        {WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENDER_DOMAIN,
         (char *)"example.com", 0},
    };

    g_assert_true (init_ordinary (&request, subject_criteria,
        G_N_ELEMENTS (subject_criteria), &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_message_search_request_init (&request,
        "account-2", "mailbox-other",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 99, domain, 1, &error));
    g_assert_no_error (error);

    g_assert_cmpstr (request.account_identity, ==, "account-2");
    g_assert_cmpstr (request.mailbox_id, ==, "mailbox-other");
    g_assert_cmpuint (request.uid_validity, ==, 99);
    g_assert_cmpuint (request.n_criteria, ==, 1);
    g_assert_cmpstr (request.criteria[0].text, ==, "example.com");
}

static void
test_message_search_request_rejects_missing_account (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

    g_assert_false (wyrebox_daemon_message_search_request_init (&request,
        NULL, "mailbox-inbox", WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77,
        NULL, 0, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null (request.account_identity);
}

static void
test_message_search_request_rejects_missing_mailbox_id (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

    g_assert_false (wyrebox_daemon_message_search_request_init (&request,
        "account-1", "", WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, NULL,
        0, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null (request.mailbox_id);
}

static void
test_message_search_request_rejects_zero_uid_validity (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

    g_assert_false (wyrebox_daemon_message_search_request_init (&request,
        "account-1", "mailbox-inbox",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 0, NULL, 0, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_cmpuint (request.uid_validity, ==, 0);
}

static void
test_message_search_request_rejects_unknown_namespace_kind (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

    g_assert_false (wyrebox_daemon_message_search_request_init (&request,
        "account-1", "mailbox-inbox", (WyreboxDaemonMailboxListEntryKind)99,
        77, NULL, 0, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null (request.account_identity);
}

static void
test_message_search_request_rejects_too_many_criteria (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
    WyreboxDaemonMessageSearchCriterion
        criteria[WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_CRITERIA + 1];

    for (guint i = 0; i < G_N_ELEMENTS (criteria); i++) {
        criteria[i].kind = WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_SINCE;
        criteria[i].text = NULL;
        criteria[i].unix_us = i;
    }

    g_assert_true (init_ordinary (&request, criteria,
        WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_CRITERIA, &error));
    g_assert_no_error (error);
    wyrebox_daemon_message_search_request_clear (&request);

    g_assert_false (init_ordinary (&request, criteria, G_N_ELEMENTS (criteria),
        &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null (request.criteria);
}

static void
test_message_search_request_rejects_invalid_text (void)
{
    char too_long[WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_TEXT_LENGTH + 2];
    const char *invalid_texts[] = {
        NULL, "", "in\nbox", "\xff\xfe", too_long,
    };

    memset (too_long, 'a', sizeof (too_long) - 1);
    too_long[sizeof (too_long) - 1] = '\0';

    for (guint i = 0; i < G_N_ELEMENTS (invalid_texts); i++) {
        WyreboxDaemonMessageSearchCriterion criterion = {
            WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_FROM_CONTAINS,
            (char *)invalid_texts[i], 0
        };

        assert_rejects_criterion (&criterion);
    }
}

static void
test_message_search_request_accepts_maximum_text_length (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
    char text[WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_TEXT_LENGTH + 1];
    WyreboxDaemonMessageSearchCriterion criterion = {
        WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SUBJECT_CONTAINS, text, 0
    };

    memset (text, 'a', sizeof (text) - 1);
    text[sizeof (text) - 1] = '\0';

    g_assert_true (init_ordinary (&request, &criterion, 1, &error));
    g_assert_no_error (error);
}

static void
test_message_search_request_rejects_text_on_date_criterion (void)
{
    WyreboxDaemonMessageSearchCriterion criterion = {
        WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_BEFORE, (char *)"x", 1
    };

    assert_rejects_criterion (&criterion);
}

static void
test_message_search_request_rejects_unknown_criterion_kind (void)
{
    WyreboxDaemonMessageSearchCriterion criterion = {
        (WyreboxDaemonMessageSearchCriterionKind)99, (char *)"x", 0
    };

    assert_rejects_criterion (&criterion);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/message-search-request/copies-fields",
        test_message_search_request_copies_fields);
    g_test_add_func ("/daemon-api/message-search-request/accepts-no-criteria",
        test_message_search_request_accepts_no_criteria);
    g_test_add_func ("/daemon-api/message-search-request/reinitializes",
        test_message_search_request_reinitializes);
    g_test_add_func ("/daemon-api/message-search-request/"
        "rejects-missing-account",
        test_message_search_request_rejects_missing_account);
    g_test_add_func ("/daemon-api/message-search-request/"
        "rejects-missing-mailbox-id",
        test_message_search_request_rejects_missing_mailbox_id);
    g_test_add_func ("/daemon-api/message-search-request/"
        "rejects-zero-uid-validity",
        test_message_search_request_rejects_zero_uid_validity);
    g_test_add_func ("/daemon-api/message-search-request/"
        "rejects-unknown-namespace-kind",
        test_message_search_request_rejects_unknown_namespace_kind);
    g_test_add_func ("/daemon-api/message-search-request/"
        "rejects-too-many-criteria",
        test_message_search_request_rejects_too_many_criteria);
    g_test_add_func ("/daemon-api/message-search-request/"
        "rejects-invalid-text",
        test_message_search_request_rejects_invalid_text);
    g_test_add_func ("/daemon-api/message-search-request/"
        "accepts-maximum-text-length",
        test_message_search_request_accepts_maximum_text_length);
    g_test_add_func ("/daemon-api/message-search-request/"
        "rejects-text-on-date-criterion",
        test_message_search_request_rejects_text_on_date_criterion);
    g_test_add_func ("/daemon-api/message-search-request/"
        "rejects-unknown-criterion-kind",
        test_message_search_request_rejects_unknown_criterion_kind);

    return g_test_run ();
}
