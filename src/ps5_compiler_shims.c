#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Shims for Mesa/ACO dependencies when running in PS5 native payload environment. */
#if defined(__PROSPERO__) || defined(__prospero__) || defined(__ORBIS__) || defined(__PS5__) || defined(OPENGNM_PSBC_ORBIS)
FILE *open_memstream(char **bufp, size_t *sizep)
{
    if (bufp) *bufp = NULL;
    if (sizep) *sizep = 0;
    errno = ENOSYS;
    return NULL;
}

__attribute__((weak)) bool
util_set_thread_affinity(void *thread, const uint32_t *mask,
                         uint32_t *old_mask, unsigned num_mask_bits)
{
    (void)thread; (void)mask;
    if (old_mask) memset(old_mask, 0, (num_mask_bits + 7u) / 8u);
    return false;
}

void _mesa_log_multiline(int level, const char *tag, const char *lines)
{
    (void)level; (void)tag; (void)lines;
}
#endif
