/* Link-time facades only. Runtime resolves the actual FW driver exports.
 * Not linked into the application as implementations or successful fallbacks.
 * Set/Get/GetHs NIDs: XlNp7jzGiPo, e-YMQ+2tj9M, r28hEh6cNH0;
 * verified against exported function bytes. */
#include <stdint.h>
int32_t sceAgcDriverSetTFRing(uint64_t address, uint32_t size)
{ (void)address; (void)size; return -1; }
int32_t sceAgcDriverGetTFRing(uint64_t *address, uint32_t *size)
{ (void)address; (void)size; return -1; }
int32_t sceAgcDriverGetHsOffchipParam(uint16_t *first, uint16_t *second)
{ (void)first; (void)second; return -1; }
