/*------------------------------------------------------------------------
 * PlayStation 5 thread-local destructor support for the native CTS payload.
 * ------------------------------------------------------------------------
 *
 * libc++abi implements __cxa_thread_atexit() on top of a weak reference to
 * __cxa_thread_atexit_impl(). The PS5 payload SDK does not export that symbol
 * from any public stub, so it must be provided by the payload itself.
 *
 * This is ordinary platform adaptation (the upstream test bodies, oracles and
 * the framework/glslang/SPIRV-Tools libraries are unmodified); without it the
 * process cannot register thread-local destructors at all.
 *
 * The destructors are registered on a pthread key whose destructor runs when a
 * thread exits normally, which covers every thread the CTS framework starts.
 * They are not run for the main thread during process exit; the payload only
 * terminates after the test session has already finalized its report.
 *//*--------------------------------------------------------------------*/

#include <pthread.h>
#include <stdlib.h>

namespace
{

struct DtorNode
{
    DtorNode *next;
    void (*func)(void *);
    void *obj;
};

pthread_key_t g_key;
pthread_once_t g_once = PTHREAD_ONCE_INIT;

void runDestructors(void *head)
{
    DtorNode *node = static_cast<DtorNode *>(head);
    while (node)
    {
        DtorNode *next = node->next;
        node->func(node->obj);
        free(node);
        node = next;
    }
}

void createKey(void)
{
    if (pthread_key_create(&g_key, runDestructors) != 0)
    {
        /* Fall through: registration below degrades to a leak rather than
           aborting the test run. */
        g_key = (pthread_key_t)-1;
    }
}

} // anonymous namespace

extern "C" int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dsoHandle)
{
    (void)dsoHandle;

    pthread_once(&g_once, createKey);
    if (g_key == (pthread_key_t)-1)
        return 0;

    DtorNode *node = static_cast<DtorNode *>(malloc(sizeof(DtorNode)));
    if (!node)
        return -1;

    node->func = func;
    node->obj  = obj;
    node->next = static_cast<DtorNode *>(pthread_getspecific(g_key));
    pthread_setspecific(g_key, node);
    return 0;
}
