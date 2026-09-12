/* Link-time facade only, resolved from libSceAgc at runtime. Not a runtime
 * implementation or a successful fallback. See Xash3D's identical ABI. */
#include <stdint.h>
/* Link-time import only; native implementation is in libSceAgc. */
__attribute__((weak)) int32_t sceAgcSuspendPoint(void) { return -1; }
uint32_t *sceAgcDcbDrawIndex(void *writer,uint32_t count,const void *indices,uint64_t modifier)
{ (void)writer;(void)count;(void)indices;(void)modifier;return 0; }
