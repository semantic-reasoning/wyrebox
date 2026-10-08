#include <chronoid/uuidv7.h>

#include <glib.h>
#include <string.h>

static void
test_uuidv7_round_trip (void)
{
    chronoid_uuidv7_t id = { 0 };
    chronoid_uuidv7_t parsed = { 0 };
    char text[CHRONOID_UUIDV7_STRING_LEN + 1] = { 0 };

    g_assert_cmpint (chronoid_uuidv7_new (&id), ==, CHRONOID_UUIDV7_OK);
    g_assert_cmpint (chronoid_uuidv7_version (&id), ==, 7);
    g_assert_false (chronoid_uuidv7_is_nil (&id));

    chronoid_uuidv7_format (&id, text);
    g_assert_cmpuint (strlen (text), ==, CHRONOID_UUIDV7_STRING_LEN);
    g_assert_cmpint (chronoid_uuidv7_parse (&parsed, text,
        CHRONOID_UUIDV7_STRING_LEN), ==, CHRONOID_UUIDV7_OK);
    g_assert_cmpint (chronoid_uuidv7_compare (&id, &parsed), ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/build-config/libchronoid/uuidv7-round-trip",
        test_uuidv7_round_trip);

    return g_test_run ();
}
