/*------------------------------------------------------------------------
 * PlayStation 5 dladdr() back-end stub for the native CTS payload.
 * ------------------------------------------------------------------------
 *
 * The SDK's static libc.a implements dladdr() on top of a weak reference to
 * __dladdr(), which is normally supplied by the dynamic loader. Payloads are
 * self-contained and the loader does not export it, and ps5-native-tool refuses
 * to link an executable that leaves the reference unresolved.
 *
 * The CTS uses dladdr() only for symbolised backtraces in optional debug
 * helpers. Reporting "no information" is the documented failure result and
 * keeps the built-in error handler working without pulling in the loader.
 *//*--------------------------------------------------------------------*/

/* Dl_info is only visible with __BSD_VISIBLE; the reference is unmangled C, so
   the parameter can stay opaque here. */
extern "C" int __dladdr(const void *addr, void *info)
{
    (void)addr;
    (void)info;
    return 0;
}
