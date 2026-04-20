// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0.

#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nx/sdk/analytics/helpers/consuming_device_agent.h>
#include <nx/sdk/analytics/i_uncompressed_video_frame.h>
#include <nx/sdk/helpers/uuid_helper.h>
#include <nx/sdk/uuid.h>

#include "ipc_client.h"

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

class Engine;

class DeviceAgent: public nx::sdk::analytics::ConsumingDeviceAgent
{
public:
    DeviceAgent(const nx::sdk::IDeviceInfo* deviceInfo, Engine* engine);
    virtual ~DeviceAgent() override;

    // Called by Engine when engine-level settings (runtime / device /
    // workerCount) change, so we push a fresh config to the worker without
    // waiting for the user to touch camera settings.
    void onEngineConfigChanged();

protected:
    virtual std::string manifestString() const override;

    virtual bool pushUncompressedVideoFrame(
        const nx::sdk::analytics::IUncompressedVideoFrame* videoFrame) override;

    virtual void doSetNeededMetadataTypes(
        nx::sdk::Result<void>* outValue,
        const nx::sdk::analytics::IMetadataTypes* neededMetadataTypes) override;

    virtual nx::sdk::Result<const nx::sdk::ISettingsResponse*> settingsReceived() override;

    virtual void doGetSettingsOnActiveSettingChange(
        nx::sdk::Result<const nx::sdk::IActiveSettingChangedResponse*>* outResult,
        const nx::sdk::IActiveSettingChangedAction* activeSettingChangedAction) override;

private:
    void sendConfigToWorker();

    Engine* m_engine = nullptr;
    std::string m_cameraUuid;
    std::unique_ptr<IpcClient> m_ipc;
    int m_frameCount = 0;
    int m_settingsCount = 0;
    int m_emittedPackets = 0;

    // Per-camera settings mirror of what we last pushed to the worker. Guarded
    // by m_settingsMutex so onEngineConfigChanged (fired from Engine thread)
    // and settingsReceived (fired from the server's settings thread) don't
    // race on the string copy.
    mutable std::mutex m_settingsMutex;
    std::string m_modelPath;
    float m_conf = 0.30f;
    float m_iou = 0.50f;
    int m_frameSkip = 1;
    // Tracker params pushed to the Python worker. Keep defaults in sync
    // with tracker.py's ByteTrackLite defaults and the manifest's
    // defaultValue fields in engine.cpp — drift between the three is a
    // classic source of "works on fresh install, breaks after upgrade"
    // bugs, so if you change one, grep the other two.
    int m_trackMaxAge = 90;
    std::string m_trackClassMatch = "vehicle_group";
    std::string m_runtime;  // populated from settingValue("runtime"); falls back to m_engine->runtime() if empty
    std::string m_device;   // same as above for "device"
    // Class name list the user ticked in the "Detect classes" CheckBoxGroup.
    // Empty vector means "user saved an empty selection" (emit nothing);
    // the absence-of-key vs empty-list distinction is re-encoded at the IPC
    // boundary — see sendConfigToWorker().
    std::vector<std::string> m_classes;
    bool m_classesSet = false; // false = never populated, send no "classes" key

    // Region polygons. Each vector is a list of [x,y] points with both
    // coords normalized to 0-1 (origin top-left). Empty vector = user did
    // not draw that polygon, worker treats as "not set" (full frame
    // positive, no exclusion).
    std::vector<std::pair<float, float>> m_roi;
    std::vector<std::pair<float, float>> m_inclusiveMask;
    std::vector<std::pair<float, float>> m_exclusiveMask;

    // Worker-assigned int track_id → NX-side UUID cache. A given physical
    // object keeps the same int across frames while alive in the tracker;
    // we hand NX a stable UUID for the same int so Desktop Client can
    // render a continuous track.
    //
    // Threading: only touched by pushUncompressedVideoFrame, which is
    // called on the mediaserver's dedicated plugin thread. No mutex
    // needed today. **If inference is ever made async (e.g. a dispatched
    // worker pool) this map MUST move under a mutex.**
    //
    // LRU eviction: an unbounded map would grow for the lifetime of the
    // camera — at ~50 B/entry, a busy street cam producing 1 new track/s
    // would hit ~5 MB/day. Bounded list + iterator-map gives O(1) lookup
    // and O(1) eviction of the least-recently-seen entry. Cap sized for
    // "hundreds of concurrent tracks + a few seconds of churn" which
    // covers every realistic CCTV scene.
    static constexpr size_t kTrackUuidCap = 4096;
    struct TrackUuidEntry { int tid; nx::sdk::Uuid uuid; };
    std::list<TrackUuidEntry> m_trackUuidLru;           // front = most-recent
    std::unordered_map<int, std::list<TrackUuidEntry>::iterator> m_trackUuids;

    // Returns the cached UUID for `tid`, or a fresh random UUID if this is
    // the first time we've seen `tid`. Either way the entry is moved to
    // the front of the LRU. `*outIsNew` is set to true iff a fresh UUID
    // was allocated on this call (used only for observability logging).
    nx::sdk::Uuid resolveTrackUuid(int tid, bool* outIsNew);
};

}}}} // namespace
