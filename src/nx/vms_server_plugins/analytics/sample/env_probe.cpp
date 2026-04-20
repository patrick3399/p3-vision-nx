// Copyright 2026 Patrick3399. Licensed under MIT (see LICENSE in repo root).

#include "env_probe.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>

#include <nx/kit/debug.h>
#include <nx/kit/json.h>

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

namespace {

// Linux-only deployment paths. A Windows build would swap these for
// %ProgramData%\p3-vision-nx\venv\Scripts\python.exe plus the Windows probe.
constexpr const char* kVenvPython   = "/opt/p3-vision-nx/venv/bin/python3";
constexpr const char* kSystemPython = "/usr/bin/python3";
constexpr const char* kProbeScript  = "/opt/p3-vision-nx/python/env_probe.py";

bool fileExists(const char* path)
{
    struct stat st;
    return ::stat(path, &st) == 0;
}

std::string readAll(FILE* pipe)
{
    std::string out;
    std::array<char, 4096> buf;
    while (true)
    {
        const size_t n = std::fread(buf.data(), 1, buf.size(), pipe);
        if (n == 0) break;
        out.append(buf.data(), n);
    }
    return out;
}

} // namespace

EnvProbe probeEnvironment()
{
    EnvProbe result;

    const char* python = fileExists(kVenvPython) ? kVenvPython : kSystemPython;

    if (!fileExists(kProbeScript))
    {
        result.errorMessage = std::string("probe script missing: ") + kProbeScript;
        NX_PRINT << "[NXCUSTOM] " << result.errorMessage;
        return result;
    }

    std::ostringstream cmd;
    cmd << python << " " << kProbeScript << " 2>/dev/null";

    NX_PRINT << "[NXCUSTOM] probeEnvironment: " << cmd.str();

    FILE* pipe = ::popen(cmd.str().c_str(), "r");
    if (!pipe)
    {
        result.errorMessage = "popen() failed";
        NX_PRINT << "[NXCUSTOM] " << result.errorMessage;
        return result;
    }

    result.rawJson = readAll(pipe);
    const int status = ::pclose(pipe);
    if (status != 0)
    {
        NX_PRINT << "[NXCUSTOM] probe exit status = " << status
                 << " (continuing with captured stdout)";
    }

    std::string err;
    auto json = nx::kit::Json::parse(result.rawJson, err);
    if (!err.empty() || !json.is_object())
    {
        result.errorMessage = "JSON parse failed: " + err
            + " raw=[" + result.rawJson + "]";
        NX_PRINT << "[NXCUSTOM] " << result.errorMessage;
        return result;
    }

    for (const auto& r : json["runtimes"].array_items())
    {
        if (r.is_string())
            result.runtimes.push_back(r.string_value());
    }

    for (const auto& kv : json["devices"].object_items())
    {
        std::vector<std::string> devs;
        for (const auto& d : kv.second.array_items())
        {
            if (d.is_string())
                devs.push_back(d.string_value());
        }
        result.devicesByRuntime[kv.first] = std::move(devs);
    }

    NX_PRINT << "[NXCUSTOM] env probe: " << result.runtimes.size() << " runtimes";
    for (const auto& rt : result.runtimes)
    {
        std::string line;
        for (const auto& d : result.devicesByRuntime[rt])
            line += (line.empty() ? "" : ",") + d;
        NX_PRINT << "[NXCUSTOM]   " << rt << " -> [" << line << "]";
    }
    return result;
}

}}}} // namespace
