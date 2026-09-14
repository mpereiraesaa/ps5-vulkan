#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../cts/upstream/log_sink_ps5.hpp"

namespace
{

std::vector<std::string> records;
bool fail_qpa;

bool contains(const char *needle)
{
    for (const std::string &record : records)
        if (record.find(needle) != std::string::npos)
            return true;
    return false;
}

size_t count(const char *needle)
{
    size_t result = 0;
    for (const std::string &record : records)
        if (record.find(needle) != std::string::npos)
            result++;
    return result;
}

void reset(void)
{
    records.clear();
    fail_qpa = false;
    cts_qpa_sink_init("run-1", "selection", "self");
}

void test_fragmented_writes_are_one_authenticated_stream(void)
{
    reset();
    FILE *stream = __wrap_fopen("TestResults.qpa", "wb");
    assert(stream != NULL);

    assert(fprintf(stream, "%s", "a") == 1);
    assert(fputs("b", stream) >= 0);
    assert(fputc('b', stream) == 'b');
    assert(fflush(stream) == 0);
    assert(fseek(stream, 0, SEEK_END) == 0);
    assert(fwrite("c", 1, 1, stream) == 1);
    assert(fclose(stream) == 0);
    assert(cts_qpa_sink_wait_completion() == 0);

    assert(count("UPSTREAM_CTS_START") == 1);
    assert(contains("run_id=run-1 selection_hash=selection eboot_sha256=self"));
    assert(count("QPA:CHUNK") == 1);
    assert(contains("QPA:CHUNK seq=0 size=4 data=YWJiYw=="));
    assert(contains("UPSTREAM_CTS_END chunks=1 total_bytes=4 "
                    "sha256=762cb46ea72a4df5e18d8a546724d854dce14331bbe59244c5ed6a94253da133"));
}

void test_chunk_boundary_is_independent_of_stdio_writes(void)
{
    reset();
    FILE *stream = __wrap_fopen("capture.qpa", "wb");
    assert(stream != NULL);

    char payload[400];
    memset(payload, 'x', sizeof(payload));
    assert(fwrite(payload, 1, 137, stream) == 137);
    assert(fwrite(payload + 137, 1, sizeof(payload) - 137, stream) == sizeof(payload) - 137);
    assert(fclose(stream) == 0);

    assert(count("QPA:CHUNK") == 2);
    assert(contains("QPA:CHUNK seq=0 size=384"));
    assert(contains("QPA:CHUNK seq=1 size=16"));
    assert(contains("UPSTREAM_CTS_END chunks=2 total_bytes=400 "
                    "sha256=7b0bd700ce066ef35190fde2dd7a0bcce426b8e10e4d32613ab550105545faad"));
}

void test_second_stream_is_rejected_fail_closed(void)
{
    reset();
    FILE *first = __wrap_fopen("first.qpa", "wb");
    assert(first != NULL);

    errno = 0;
    FILE *second = __wrap_fopen("second.qpa", "wb");
    assert(second == NULL);
    assert(errno == EBUSY);
    assert(contains("QPA sink rejects a second log stream"));
    assert(fclose(first) == 0);
}

void test_transport_failure_cannot_emit_success_footer(void)
{
    reset();
    FILE *stream = __wrap_fopen("TestResults.qpa", "wb");
    assert(stream != NULL);

    char payload[384];
    memset(payload, 'z', sizeof(payload));
    fail_qpa = true;
    assert(fwrite(payload, 1, sizeof(payload), stream) == 0);
    assert(fclose(stream) == EOF);
    assert(cts_qpa_sink_wait_completion() != 0);

    assert(!contains("UPSTREAM_CTS_END"));
    assert(contains("QPA sink incomplete completed=1 failed=1"));
}

void test_non_append_seek_is_rejected_fail_closed(void)
{
    reset();
    FILE *stream = __wrap_fopen("TestResults.qpa", "wb");
    assert(stream != NULL);

    errno = 0;
    assert(fseek(stream, 0, SEEK_SET) == -1);
    assert(errno == EINVAL);
    assert(fclose(stream) == EOF);
    assert(cts_qpa_sink_wait_completion() != 0);
    assert(!contains("UPSTREAM_CTS_END"));
}

} // namespace

extern "C" int ps5log_printf(const char *level, const char *format, ...)
{
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    records.push_back(std::string(level ? level : "") + ":" + text);
    return fail_qpa && level && strcmp(level, "QPA") == 0 ? -1 : 0;
}

int main(void)
{
    test_fragmented_writes_are_one_authenticated_stream();
    test_chunk_boundary_is_independent_of_stdio_writes();
    test_second_stream_is_rejected_fail_closed();
    test_transport_failure_cannot_emit_success_footer();
    test_non_append_seek_is_rejected_fail_closed();
    puts("Bounded synchronous QPA sink: pass");
    return 0;
}
