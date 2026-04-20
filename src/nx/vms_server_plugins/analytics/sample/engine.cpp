// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0.

#include "engine.h"
#include "device_agent.h"
#include "coco80.h"

#include <algorithm>
#include <set>
#include <sstream>

#include <nx/kit/debug.h>
#include <nx/sdk/helpers/settings_response.h>

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

using namespace nx::sdk;
using namespace nx::sdk::analytics;

namespace {

std::string jsonStringArray(const std::vector<std::string>& items)
{
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i) os << ",";
        os << "\"" << items[i] << "\"";
    }
    os << "]";
    return os.str();
}

int parseInt(const std::string& s, int fallback)
{
    if (s.empty()) return fallback;
    try { return std::stoi(s); }
    catch (...) { return fallback; }
}

} // namespace

Engine::Engine(): nx::sdk::analytics::Engine(/*enableOutput*/ true)
{
    m_env = probeEnvironment();
    initDefaultsFromProbe();
    buildManifest();
}

Engine::~Engine() {}

void Engine::doObtainDeviceAgent(Result<IDeviceAgent*>* outResult, const IDeviceInfo* deviceInfo)
{
    *outResult = new DeviceAgent(deviceInfo, this);
}

std::string Engine::manifestString() const
{
    return m_manifestCache;
}

std::string Engine::runtime() const
{
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_runtime;
}

std::string Engine::device() const
{
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_device;
}

int Engine::workerCount() const
{
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    return m_workerCount;
}

void Engine::registerAgent(DeviceAgent* agent)
{
    if (!agent) return;
    std::lock_guard<std::mutex> lock(m_agentsMutex);
    m_agents.push_back(agent);
}

void Engine::unregisterAgent(DeviceAgent* agent)
{
    if (!agent) return;
    std::lock_guard<std::mutex> lock(m_agentsMutex);
    m_agents.erase(std::remove(m_agents.begin(), m_agents.end(), agent), m_agents.end());
}

void Engine::initDefaultsFromProbe()
{
    // Default runtime = first reported runtime, default device = first device
    // under that runtime. If the probe came back empty, leave placeholders
    // that match the manifest's "<none>" sentinel so the UI still renders.
    if (!m_env.runtimes.empty())
        m_runtime = m_env.runtimes.front();
    else
        m_runtime = "<none>";

    auto it = m_env.devicesByRuntime.find(m_runtime);
    if (it != m_env.devicesByRuntime.end() && !it->second.empty())
        m_device = it->second.front();
    else
        m_device = "<none>";

    m_workerCount = 1;
}

Result<const ISettingsResponse*> Engine::settingsReceived()
{
    std::string newRuntime, newDevice;
    int newWorkerCount = 1;

    // helpers::Engine::settingValue() reads the persisted value by name.
    newRuntime = settingValue("runtime");
    newDevice = settingValue("device");
    const std::string wcStr = settingValue("workerCount");
    newWorkerCount = parseInt(wcStr, 1);

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        if (!newRuntime.empty() && newRuntime != m_runtime) { m_runtime = newRuntime; changed = true; }
        if (!newDevice.empty()  && newDevice  != m_device)  { m_device  = newDevice;  changed = true; }
        if (newWorkerCount != m_workerCount) { m_workerCount = newWorkerCount; changed = true; }
    }

    NX_PRINT << "[NXCUSTOM] engine settingsReceived runtime=" << newRuntime
             << " device=" << newDevice << " workerCount=" << newWorkerCount
             << " changed=" << (changed ? 1 : 0);

    if (changed)
        broadcastEngineConfig();
    return nullptr;
}

void Engine::broadcastEngineConfig()
{
    std::vector<DeviceAgent*> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_agentsMutex);
        snapshot = m_agents;
    }
    NX_PRINT << "[NXCUSTOM] broadcastEngineConfig → " << snapshot.size() << " agents";
    for (auto* agent: snapshot)
    {
        if (agent)
            agent->onEngineConfigChanged();
    }
}

void Engine::buildManifest()
{
    std::vector<std::string> runtimes = m_env.runtimes;
    if (runtimes.empty())
        runtimes.push_back("<none>");

    // Flat union of devices across runtimes. A future revision could narrow
    // the list to the selected runtime via an Active Setting round-trip; the
    // current build keeps the union so the user sees every installed device.
    std::vector<std::string> deviceUnion;
    std::set<std::string> seen;
    for (const auto& kv : m_env.devicesByRuntime)
    {
        for (const auto& d : kv.second)
        {
            if (seen.insert(d).second)
                deviceUnion.push_back(d);
        }
    }
    if (deviceUnion.empty())
        deviceUnion.push_back("<none>");

    const std::string runtimeRange = jsonStringArray(runtimes);
    const std::string deviceRange  = jsonStringArray(deviceUnion);
    const std::string runtimeDefault = runtimes.front();
    const std::string deviceDefault  = deviceUnion.front();

    std::ostringstream m;
    m << R"json({
    "capabilities": "needUncompressedVideoFrames_rgb",
    "engineSettingsModel":
    {
        "type": "Settings",
        "items":
        [
            {
                "type": "GroupBox",
                "caption": "Inference",
                "items":
                [
                    { "type": "ComboBox",
                      "name": "runtime",
                      "caption": "Inference Runtime",
                      "defaultValue": ")json" << runtimeDefault << R"json(",
                      "range": )json" << runtimeRange << R"json( },
                    { "type": "ComboBox",
                      "name": "device",
                      "caption": "Device",
                      "defaultValue": ")json" << deviceDefault << R"json(",
                      "range": )json" << deviceRange << R"json( },
                    { "type": "SpinBox",
                      "name": "workerCount",
                      "caption": "Python worker count",
                      "defaultValue": 1,
                      "minValue": 1,
                      "maxValue": 4 }
                ]
            }
        ]
    },
    "deviceAgentSettingsModel":
    {
        "type": "Settings",
        "items":
        [
            {
                "type": "GroupBox",
                "caption": "Inference",
                "items":
                [
                    { "type": "ComboBox",
                      "name": "runtime",
                      "caption": "Inference Runtime",
                      "description": "Inference backend for this camera. The list shows only the runtimes actually installed on this server.",
                      "defaultValue": ")json" << runtimeDefault << R"json(",
                      "range": )json" << runtimeRange << R"json( },
                    { "type": "ComboBox",
                      "name": "device",
                      "caption": "Device",
                      "description": "Target device for the selected runtime. Combinations that do not apply to the chosen runtime fall back to AUTO.",
                      "defaultValue": ")json" << deviceDefault << R"json(",
                      "range": )json" << deviceRange << R"json( }
                ]
            },
            {
                "type": "GroupBox",
                "caption": "Model",
                "items":
                [
                    { "type": "TextField",
                      "name": "modelPath",
                      "caption": "Model file path",
                      "description": "Accepts .pt / .onnx / .engine / *_openvino_model dir. Must be readable by the mediaserver user (Linux: networkoptix, Windows: LocalSystem).",
                      "defaultValue": "/var/lib/p3-vision-nx/models/yolo26n.onnx" },
                    { "type": "Button",
                      "name": "validate",
                      "caption": "Validate model",
                      "isActive": true }
                ]
            },
            {
                "type": "GroupBox",
                "caption": "Thresholds",
                "items":
                [
                    { "type": "SpinBox",
                      "name": "conf",
                      "caption": "Confidence %",
                      "defaultValue": 30,
                      "minValue": 1,
                      "maxValue": 99 },
                    { "type": "SpinBox",
                      "name": "iou",
                      "caption": "IoU %",
                      "defaultValue": 50,
                      "minValue": 1,
                      "maxValue": 99 },
                    { "type": "SpinBox",
                      "name": "frameSkip",
                      "caption": "Frame skip",
                      "defaultValue": 1,
                      "minValue": 1,
                      "maxValue": 30 }
                ]
            },
            {
                "type": "GroupBox",
                "caption": "Classes",
                "items":
                [
                    { "type": "Button",
                      "name": "classesSelectAll",
                      "caption": "Select all",
                      "description": "Tick every COCO-80 class.",
                      "isActive": true },
                    { "type": "Button",
                      "name": "classesDeselectAll",
                      "caption": "Deselect all",
                      "description": "Untick every class. The worker short-circuits when zero classes are selected — no inference runs, no detections emitted.",
                      "isActive": true },
                    { "type": "CheckBoxGroup",
                      "name": "classes",
                      "caption": "Detect classes",
                      "description": "COCO-80 classes. Each ticked class is emitted as a distinct object type with its own label in Desktop Client. Unticking every class stops inference entirely (no model run, zero CPU / GPU load).",
                      "range": )json" << allClassesJsonArray() << R"json(,
                      "defaultValue": ["person"] }
                ]
            },
            {
                "type": "GroupBox",
                "caption": "Regions",
                "items":
                [
                    { "type": "BoxFigure",
                      "name": "roi",
                      "caption": "ROI (only detect inside)",
                      "description": "Axis-aligned rectangle; aligns with NX AI Manager convention. For irregular shapes use Inclusive mask below." },
                    { "type": "PolygonFigure",
                      "name": "inclusiveMask",
                      "caption": "Inclusive mask",
                      "description": "Polygon that adds to the detection area. Combined with ROI as (ROI \u222a Inclusive).",
                      "minPoints": 3,
                      "maxPoints": 12 },
                    { "type": "PolygonFigure",
                      "name": "exclusiveMask",
                      "caption": "Exclusive mask",
                      "description": "Polygon that subtracts from the detection area (e.g. a TV playing news to avoid false positives).",
                      "minPoints": 3,
                      "maxPoints": 12 }
                ]
            }
        ]
    },
    "typeLibrary":
    {
        "eventTypes": [],
        "objectTypes": )json" << typeLibraryObjectTypesJson() << R"json(
    }
})json";
    m_manifestCache = m.str();
    NX_PRINT << "[NXCUSTOM] engine manifest built, length="
             << m_manifestCache.size() << " bytes, runtimes=" << runtimes.size()
             << " devices=" << deviceUnion.size()
             << " default_runtime=" << runtimeDefault
             << " default_device=" << deviceDefault;
}

}}}} // namespace
