// Copyright 2026 patrick. Licensed under MPL 2.0.

#pragma once

#include <map>
#include <string>
#include <vector>

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

struct EnvProbe
{
    std::vector<std::string> runtimes;
    std::map<std::string, std::vector<std::string>> devicesByRuntime;
    std::string rawJson;
    std::string errorMessage;
};

/**
 * Spawn env_probe.py with the packaged venv Python, capture JSON on stdout,
 * parse it, and return. On any failure returns an EnvProbe with empty
 * runtimes and errorMessage populated — callers must still render a usable
 * manifest (inject "<none>" items).
 */
EnvProbe probeEnvironment();

}}}} // namespace
