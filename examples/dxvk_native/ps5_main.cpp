/* PS5 entry point of the DXVK native payload.
 *
 * The pinned DXVK D3D11/DXGI objects and ps5vk are linked statically into
 * this executable. The entry point opens the ps5log/1 channel, reports the
 * build identity (including the SHA-256 of the running eboot), routes DXVK's
 * logger into ps5log records, runs the offscreen D3D11 workload, reports the
 * first refusal and waits for Close Game. Nothing is written to the console's
 * filesystem: DXVK file logging and the state cache are disabled through the
 * process environment seen by DXVK. */
#include "build_identity.h"
#include "compat_layer.h"
#include "sha256.h"
#include "telemetry.h"
#include "workload.h"

#include "ps5log.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <iostream>
#include <streambuf>
#include <string>

extern "C" {
/* Link-time wrappers (--wrap): see tools/build_dxvk_ps5_native.py. */
char *__real_getenv(const char *name);
char *__wrap_getenv(const char *name);
int __real_ps5log_line(const char *level, const char *text);
int __wrap_ps5log_line(const char *level, const char *text);
int __wrap_ps5log_printf(const char *level, const char *fmt, ...);
int __real_ps5log_hex64(const char *level, const char *label, uint64_t value);
int __wrap_ps5log_hex64(const char *level, const char *label, uint64_t value);
}

namespace {

/* The environment DXVK reads. DXVK_WSI_DRIVER selects the PS5 adapter; file
 * logging and the state cache stay off; a dxvk.conf is read only if packaged. */
const char *const g_environment[][2] = {
    {"DXVK_WSI_DRIVER", "PS5"},
    {"DXVK_LOG_LEVEL", "info"},
    {"DXVK_LOG_PATH", "none"},
    {"DXVK_STATE_CACHE", "disable"},
    {"DXVK_NO_VR", "1"},
    {"DXVK_CONFIG_FILE", "/app0/dxvk.conf"},
};

pthread_mutex_t *log_mutex()
{
    static pthread_mutex_t mutex;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    });
    return &mutex;
}

struct Lock {
    Lock() { dxvk_telemetry_lock(); }
    ~Lock() { dxvk_telemetry_unlock(); }
};

/* Stage stack and refusal state, guarded by the log lock. */
const char *g_stages[16];
unsigned g_stage_depth = 0;
char g_last_stage[48] = "none";
char g_last_call[64] = "none";
char g_last_params[640] = "";
bool g_have_refusal = false;
char g_refusal[900] = "";
volatile sig_atomic_t g_crashed = 0;

/* DXVK's native logger writes whole lines to std::cerr under its own mutex.
 * This stream buffer turns each line into one DXVK_LOG record. */
class LoggerSink final : public std::streambuf {
protected:
    int_type overflow(int_type c) override {
        if (c != traits_type::eof()) {
            char ch = char(c);
            xsputn(&ch, 1);
        }
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        Lock lock;
        for (std::streamsize i = 0; i < n; ++i) {
            if (s[i] == '\n') flush_line();
            else if (m_line.size() < 900) m_line.push_back(s[i]);
        }
        return n;
    }

private:
    void flush_line() {
        static const struct { const char *prefix, *level, *record; } levels[] = {
            {"trace: ", "trace", "INFO"}, {"debug: ", "debug", "INFO"},
            {"info:  ", "info", "INFO"}, {"warn:  ", "warn", "WARN"},
            {"err:   ", "err", "ERR"},
        };
        const char *level = "raw", *record = "INFO", *text = m_line.c_str();
        for (const auto &entry : levels) {
            if (!m_line.compare(0, strlen(entry.prefix), entry.prefix)) {
                level = entry.level;
                record = entry.record;
                text += strlen(entry.prefix);
                break;
            }
        }
        dxvk_telemetry_emit(record, "DXVK_LOG level=%s stage=%s text=%s", level,
                            dxvk_telemetry_open_stage(), text);
        /* Refusal candidates: any err line, a filtered adapter (warn
         * "Skipping ..."), and a missing required extension, which DXVK logs
         * at info level ("Required Vulkan extension X not supported") before
         * the generic err line. */
        if (!strcmp(level, "err") || (!strcmp(level, "warn") && !strncmp(text, "Skipping", 8)) ||
            (!strncmp(text, "Required ", 9) && strstr(text, " not supported")))
            dxvk_telemetry_refusal("dxvk_log", level, 0, text);
        m_line.clear();
    }

    std::string m_line;
};

LoggerSink g_sink;

void crash_handler(int signal, siginfo_t *info, void *)
{
    if (g_crashed) _exit(1);
    g_crashed = 1;
    char line[900];
    snprintf(line, sizeof(line),
             "DXVK_NATIVE_CRASH signal=%d addr=%p stage=%s last_stage=%s last_vk_call=%s params=%.400s",
             signal, info ? info->si_addr : nullptr,
             g_stage_depth ? g_stages[g_stage_depth - 1] : "none", g_last_stage,
             g_last_call, g_last_params);
    /* Best effort: the crashing thread may hold the log lock. */
    if (pthread_mutex_trylock(log_mutex()) == 0) {
        __real_ps5log_line("ERR", line);
        ps5log_close("dxvk-native-crash");
        pthread_mutex_unlock(log_mutex());
    }
    for (;;) sleep(1); /* Wait for the runner's Close Game. */
}

void install_crash_handler()
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        sigaction(signal, &action, nullptr);
}

void eboot_digest(char hex[65], unsigned long long *bytes)
{
    *bytes = 0;
    snprintf(hex, 65, "unavailable");
    int fd = open("/app0/eboot.bin", O_RDONLY);
    if (fd < 0) return;
    dxvk_sha256 sha;
    dxvk_sha256_init(&sha);
    static unsigned char buffer[1 << 16];
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0) { close(fd); return; }
        if (n == 0) break;
        dxvk_sha256_update(&sha, buffer, size_t(n));
        *bytes += (unsigned long long)n;
    }
    close(fd);
    dxvk_sha256_hex(&sha, hex);
}

void emit_identity()
{
    char eboot[65];
    unsigned long long bytes = 0;
    eboot_digest(eboot, &bytes);
    dxvk_telemetry_emit("MARK",
        "DXVK_NATIVE_IDENTITY variant=%s label=%s diagnostic=%d dxvk_commit=%s ps5vk_commit=%s "
        "ps5vk_dirty=%d patches=%s eboot_sha256=%s eboot_bytes=%llu vs_sha256=%s ps_sha256=%s "
        "oracle_expected_checksum=%08x compat_layer=%d",
        DXVK_NATIVE_VARIANT, DXVK_NATIVE_DIAGNOSTIC ? "DIAGNOSTIC" : "UNMODIFIED",
        DXVK_NATIVE_DIAGNOSTIC, DXVK_NATIVE_DXVK_COMMIT, DXVK_NATIVE_PS5VK_COMMIT,
        DXVK_NATIVE_PS5VK_DIRTY, DXVK_NATIVE_PATCHES, eboot, bytes, DXVK_NATIVE_VS_SHA256_BUILD,
        DXVK_NATIVE_PS_SHA256_BUILD, dxvk_oracle_expected_checksum(),
        DXVK_NATIVE_COMPAT_LAYER ? DXVK_NATIVE_COMPAT_LAYER_VERSION : 0);
    std::string env;
    for (const auto &entry : g_environment)
        env += std::string(env.empty() ? "" : ",") + entry[0] + "=" + entry[1];
    dxvk_telemetry_emit("INFO", "DXVK_NATIVE_ENVIRONMENT %s", env.c_str());
}

void on_stage(const char *stage, const char *state, const char *detail)
{
    dxvk_telemetry_stage(stage, state, detail);
}

void on_oracle(const dxvk_oracle_result *r)
{
    char first[600] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < r->reported && used < sizeof(first); ++i) {
        int n = snprintf(first + used, sizeof(first) - used, "%s%u,%u:%08x!=%08x",
                         i ? ";" : "", r->first[i].x, r->first[i].y, r->first[i].got,
                         r->first[i].expected);
        if (n < 0) break;
        used += size_t(n);
    }
    dxvk_telemetry_emit(r->mismatches ? "ERR" : "MARK",
        "DXVK_ORACLE checked=%u mismatches=%u checksum=%08x expected_checksum=%08x first=%s",
        r->checked, r->mismatches, r->checksum, r->expected_checksum, used ? first : "none");
}

} // namespace

/* ---- link-time wrappers ---- */

extern "C" char *__wrap_getenv(const char *name)
{
    if (name)
        for (const auto &entry : g_environment)
            if (!strcmp(name, entry[0])) return const_cast<char *>(entry[1]);
    return __real_getenv(name);
}

extern "C" int __wrap_ps5log_line(const char *level, const char *text)
{
    Lock lock;
    return __real_ps5log_line(level, text);
}

extern "C" int __wrap_ps5log_printf(const char *level, const char *fmt, ...)
{
    char line[PS5LOG_MAX_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    Lock lock;
    return __real_ps5log_line(level, line);
}

extern "C" int __wrap_ps5log_hex64(const char *level, const char *label, uint64_t value)
{
    Lock lock;
    return __real_ps5log_hex64(level, label, value);
}

/* ---- telemetry ---- */

extern "C" void dxvk_telemetry_lock(void) { pthread_mutex_lock(log_mutex()); }
extern "C" void dxvk_telemetry_unlock(void) { pthread_mutex_unlock(log_mutex()); }

extern "C" void dxvk_telemetry_emit(const char *level, const char *fmt, ...)
{
    char line[PS5LOG_MAX_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    Lock lock;
    __real_ps5log_line(level, line);
}

extern "C" void dxvk_telemetry_stage(const char *stage, const char *state, const char *detail)
{
    Lock lock;
    if (!strcmp(state, "begin")) {
        if (g_stage_depth < sizeof(g_stages) / sizeof(g_stages[0]))
            g_stages[g_stage_depth++] = stage;
        if (strcmp(stage, "shutdown"))
            snprintf(g_last_stage, sizeof(g_last_stage), "%s", stage);
    } else {
        /* Close the named stage; stages still open inside it end with the
         * same state, so a failure is attributed to the innermost stage. */
        for (unsigned i = g_stage_depth; i-- > 0;) {
            if (strcmp(g_stages[i], stage)) continue;
            while (g_stage_depth > i + 1) {
                const char *inner = g_stages[--g_stage_depth];
                dxvk_telemetry_emit(!strcmp(state, "fail") ? "ERR" : "MARK",
                                    "DXVK_NATIVE_STAGE stage=%s state=%s closed_by=%s",
                                    inner, state, stage);
            }
            g_stage_depth = i;
            break;
        }
    }
    dxvk_telemetry_emit(!strcmp(state, "fail") ? "ERR" : "MARK",
                        "DXVK_NATIVE_STAGE stage=%s state=%s%s%s", stage, state,
                        detail && *detail ? " " : "", detail ? detail : "");
}

extern "C" const char *dxvk_telemetry_open_stage(void)
{
    Lock lock;
    return g_stage_depth ? g_stages[g_stage_depth - 1] : "none";
}

extern "C" void dxvk_telemetry_last_call(const char *call, const char *params)
{
    Lock lock;
    snprintf(g_last_call, sizeof(g_last_call), "%s", call);
    snprintf(g_last_params, sizeof(g_last_params), "%s", params ? params : "");
}

extern "C" void dxvk_telemetry_refusal(const char *source, const char *call, int result,
                                       const char *detail)
{
    Lock lock;
    if (g_have_refusal) return;
    g_have_refusal = true;
    if (!strcmp(source, "vulkan"))
        snprintf(g_refusal, sizeof(g_refusal), "source=vulkan stage=%s call=%s result=%d params=%.600s",
                 dxvk_telemetry_open_stage(), call, result, detail ? detail : "");
    else
        snprintf(g_refusal, sizeof(g_refusal),
                 "source=%s stage=%s level=%s result=%d last_vk_call=%s last_vk_params=%.300s text=%.300s",
                 source, dxvk_telemetry_open_stage(), call, result, g_last_call, g_last_params,
                 detail ? detail : "");
}

extern "C" void dxvk_telemetry_first_refusal_summary(void)
{
    Lock lock;
    if (g_have_refusal)
        dxvk_telemetry_emit("ERR", "DXVK_FIRST_REFUSAL %s", g_refusal);
    else
        dxvk_telemetry_emit("MARK", "DXVK_FIRST_REFUSAL source=none");
}

int main(void)
{
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t boot = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    ps5log_config config;
    const char *loaded = nullptr, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if (ps5log_load_config(paths, 1, &config, &loaded)) _exit(1);
    config.udp = 0;
    if (ps5log_init(&config, "PPSA99994", "ps5vk", boot)) _exit(1);
    install_crash_handler();
    std::cerr.rdbuf(&g_sink);

    emit_identity();
    DxvkNativeHooks hooks = {on_stage, on_oracle};
    DxvkNativeSummary summary;
    int outcome = dxvk_native_run_workload(hooks, &summary);
    if (summary.create_hresult & 0x80000000u) {
        char detail[64];
        snprintf(detail, sizeof(detail), "hr=0x%08x", summary.create_hresult);
        dxvk_telemetry_refusal("d3d11", "D3D11CreateDevice", int(summary.create_hresult), detail);
    }
    std::cerr.flush();
    dxvk_trace_summary();
    dxvk_telemetry_first_refusal_summary();
    static const char *const outcomes[] = {"rendered", "refused", "oracle_mismatch"};
    dxvk_telemetry_emit(outcome == DXVK_NATIVE_RENDERED ? "MARK" : "ERR",
        "DXVK_NATIVE_RESULT outcome=%s variant=%s label=%s last_stage=%s create_hr=0x%08x "
        "feature_level=0x%04x device_refs=%u context_refs=%u",
        outcomes[outcome >= 0 && outcome <= 2 ? outcome : 1], DXVK_NATIVE_VARIANT,
        DXVK_NATIVE_DIAGNOSTIC ? "DIAGNOSTIC" : "UNMODIFIED", summary.last_stage,
        summary.create_hresult, summary.feature_level, summary.device_refs_at_release,
        summary.context_refs_at_release);
    {
        Lock lock;
        ps5log_close(outcome == DXVK_NATIVE_RENDERED ? "dxvk-native-rendered" : "dxvk-native-end");
    }
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}
