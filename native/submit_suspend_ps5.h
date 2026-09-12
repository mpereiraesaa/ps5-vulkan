#ifndef PS5VK_SUBMIT_SUSPEND_PS5_H
#define PS5VK_SUBMIT_SUSPEND_PS5_H
#include "ps5_agc_driver.h"
#include "ps5_platform.h"

/* Native import, not a fence, resource-retirement operation or success stub.
 * FW12.02 NID h9z6+0hEydk; ABI and lifecycle behavior are regression-tested. */
extern int32_t sceAgcSuspendPoint(void);
struct ps5vk_submit_result {
    int32_t submit_rc;
    int32_t suspend_rc;
    unsigned suspend_attempted;
};

/* Callers retain all submitted storage until independently proven completion.
 * A failed suspend point after a successful submit is still submitted work. */
static inline struct ps5vk_submit_result
ps5vk_submit_suspend(struct ps5_agc_submit *packet)
{
    struct ps5vk_submit_result result = {0};
    result.submit_rc = sceAgcDriverSubmitDcb(packet);
    if (!result.submit_rc) {
        result.suspend_attempted = 1;
        result.suspend_rc = sceAgcSuspendPoint();
    }
    return result;
}
#endif
