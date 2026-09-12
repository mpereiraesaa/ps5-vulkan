#include "submit_suspend_ps5.h"
#include <assert.h>
#include <stdio.h>

static unsigned calls;
static int32_t submit_rc, suspend_rc;
static struct ps5_agc_submit *expected;
int32_t sceAgcDriverSubmitDcb(void *packet)
{
    assert(calls++ == 0 && packet == expected);
    return submit_rc;
}
int32_t sceAgcSuspendPoint(void)
{
    assert(calls++ == 1);
    return suspend_rc;
}
static void check(int32_t first, int32_t second)
{
    struct ps5_agc_submit packet = {0};
    expected=&packet; calls=0; submit_rc=first; suspend_rc=second;
    struct ps5vk_submit_result result=ps5vk_submit_suspend(&packet);
    assert(result.submit_rc==first);
    assert(result.suspend_attempted==(first==0));
    assert(result.suspend_rc==(first==0 ? second : 0));
    assert(calls==(first==0 ? 2u : 1u));
}
int main(void)
{
    check(0,0);
    check(-1,0);
    check(7,-2);
    check(0,-3);
    check(0,9);
    puts("Submit/suspend ordering and errors: pass (mock only; not completion)");
}
