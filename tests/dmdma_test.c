#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmdma.h"

static dmdma_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmdma_create();
}

void dmod_test_teardown(void)
{
    dmdma_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmdma_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmdma_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmdma_is_valid(g_handle));
}

DMOD_TEST_STEP(dmdma_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmdma_destroy(NULL);
}
