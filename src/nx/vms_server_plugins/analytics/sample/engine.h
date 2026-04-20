// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0.

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <nx/sdk/analytics/helpers/engine.h>
#include <nx/sdk/analytics/helpers/plugin.h>
#include <nx/sdk/analytics/i_compressed_video_packet.h>

#include "env_probe.h"

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

class DeviceAgent;

class Engine: public nx::sdk::analytics::Engine
{
public:
    Engine();
    virtual ~Engine() override;

    const EnvProbe& envProbe() const { return m_env; }

    // Engine-level inference settings. Defaults are filled from the env
    // probe's first runtime/device and stay in sync with the manifest
    // default shown to the user.
    std::string runtime() const;
    std::string device() const;
    int workerCount() const;

    // DeviceAgent self-registers in ctor / unregisters in dtor so that an
    // engine-level setting change can fan out new config to every attached
    // camera without waiting for the user to re-apply camera settings.
    void registerAgent(DeviceAgent* agent);
    void unregisterAgent(DeviceAgent* agent);

protected:
    virtual std::string manifestString() const override;

    virtual void doObtainDeviceAgent(
        nx::sdk::Result<nx::sdk::analytics::IDeviceAgent*>* outResult,
        const nx::sdk::IDeviceInfo* deviceInfo) override;

    virtual nx::sdk::Result<const nx::sdk::ISettingsResponse*> settingsReceived() override;

private:
    void buildManifest();
    void initDefaultsFromProbe();
    void broadcastEngineConfig();

    EnvProbe m_env;
    std::string m_manifestCache;

    mutable std::mutex m_settingsMutex;
    std::string m_runtime;
    std::string m_device;
    int m_workerCount = 1;

    std::mutex m_agentsMutex;
    std::vector<DeviceAgent*> m_agents;
};

}}}} // namespace
