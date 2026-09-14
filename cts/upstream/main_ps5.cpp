#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <sstream>
#include <memory>

#include "tcuDefs.hpp"
#include "tcuCommandLine.hpp"
#include "tcuPlatform.hpp"
#include "tcuApp.hpp"
#include "tcuResource.hpp"
#include "tcuTestLog.hpp"
#include "tcuTestPackage.hpp"

#include "ps5log.h"
#include "platform_ps5.hpp"
#include "log_sink_ps5.hpp"

namespace
{

std::string readFileString(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return "";
    char buf[1024];
    std::string result;
    while (fgets(buf, sizeof(buf), f))
        result += buf;
    fclose(f);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    return result;
}

std::vector<std::string> buildArguments(int argc, char **argv)
{
    std::vector<std::string> args;
    args.push_back(argc > 0 && argv[0] ? argv[0] : "upstream_cts");

    // Check if cmdline.txt exists
    std::string cmdlineFile = readFileString("/app0/cmdline.txt");
    if (!cmdlineFile.empty())
    {
        std::istringstream iss(cmdlineFile);
        std::string token;
        while (iss >> token)
            args.push_back(token);
    }
    else if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
            args.push_back(argv[i]);
    }
    else
    {
        // Default options
        args.push_back("--deqp-log-filename=TestResults.qpa");
        args.push_back("--deqp-watchdog=disable");
        args.push_back("--deqp-crashhandler=disable");
        // The CTS shader cache preallocates a fixed 16 MiB item pool on first
        // use (cacheMaxItems = 1024 * 1024 uint32 items). Avoid this optional
        // allocation for the focused run. Application-heap mode now also fixes
        // the original internal-heap budget limit. The cache is a compile
        // cache only, so disabling it (an upstream-supported option) changes no
        // test body, oracle or result semantics.
        args.push_back("--deqp-shadercache=disable");

        // Check if /app0/cases.txt exists
        FILE *fCases = fopen("/app0/cases.txt", "r");
        if (fCases)
        {
            fclose(fCases);
            args.push_back("--deqp-caselist-file=/app0/cases.txt");
        }
        else
        {
            args.push_back("--deqp-case=dEQP-VK.api.smoke.*");
        }
    }

    return args;
}

} // anonymous namespace

int main(int argc, char **argv)
{
    // 1. Initialize TCP ps5log connection
    ps5log_quickstart("PPSA99994", "upstream-cts", PS5LOG_CAPTURE_NONE);
    ps5log_printf(PS5LOG_INFO, "PS5 Vulkan Upstream CTS Native starting");

    // 2. Load run identity and metadata
    std::string runId = readFileString("/app0/run_id.txt");
    if (runId.empty())
    {
        char autoId[64];
        snprintf(autoId, sizeof(autoId), "run-%llu", (unsigned long long)ps5log_monotonic_ns());
        runId = autoId;
    }

    std::string selectionHash = readFileString("/app0/selection_hash.txt");
    if (selectionHash.empty()) selectionHash = "unspecified";

    std::string ebootSha256 = readFileString("/app0/eboot_sha256.txt");
    if (ebootSha256.empty()) ebootSha256 = "unspecified";

    cts_qpa_sink_init(runId.c_str(), selectionHash.c_str(), ebootSha256.c_str());

    // 3. Assemble command line
    std::vector<std::string> argStrings = buildArguments(argc, argv);
    std::vector<const char *> cArgs;
    for (const auto &s : argStrings)
        cArgs.push_back(s.c_str());

    ps5log_printf(PS5LOG_INFO, "CTS arguments count: %zu", cArgs.size());
    for (size_t i = 0; i < cArgs.size(); i++)
        ps5log_printf(PS5LOG_INFO, "  arg[%zu]: %s", i, cArgs[i]);

    int exitStatus = EXIT_SUCCESS;

    try
    {
        tcu::CommandLine cmdLine((int)cArgs.size(), cArgs.data());
        tcu::DirArchive archive("/app0");
        tcu::TestLog log(cmdLine.getLogFileName(), cmdLine.getLogFlags());

        std::unique_ptr<tcu::Platform> platform(new cts::ps5::Ps5Platform());
        std::unique_ptr<tcu::App> app(new tcu::App(*platform, archive, log, cmdLine));

        ps5log_printf(PS5LOG_INFO, "Starting test iteration loop");

        while (app->iterate())
        {
            // iterate until all test cases complete
        }

        const tcu::TestRunStatus &res = app->getResult();
        ps5log_printf(PS5LOG_INFO, "Test execution complete: pass=%d fail=%d notSupported=%d total=%d",
                      res.numPassed, res.numFailed, res.numNotSupported, res.numExecuted);

        if (!res.isComplete || res.numFailed > 0)
            exitStatus = EXIT_FAILURE;

        // Destroy app and log to flush stream
        app.reset();
    }
    catch (const std::exception &e)
    {
        ps5log_printf(PS5LOG_ERR, "Fatal exception during CTS execution: %s", e.what());
        exitStatus = EXIT_FAILURE;
    }

    // A complete authenticated QPA stream is part of the device verdict.
    if (cts_qpa_sink_wait_completion() != 0)
        exitStatus = EXIT_FAILURE;

    ps5log_printf(PS5LOG_INFO, "UPSTREAM_CTS_COMPLETE status=%d", exitStatus);
    ps5log_close(exitStatus == EXIT_SUCCESS ? "complete-success" : "complete-failure");

    // Hold cleanly for supervisor close
    while (1)
    {
        sleep(1);
    }

    return exitStatus;
}
