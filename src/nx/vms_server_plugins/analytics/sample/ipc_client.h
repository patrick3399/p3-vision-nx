// Copyright 2026 Patrick3399. Licensed under MIT (see LICENSE in repo root).

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <nx/kit/json.h>

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

/**
 * IPC client for the Python worker.
 *
 * Wire format (v3):
 *   Control msg  : <uint32 BE length> <utf8 JSON payload>
 *   Frame msg    : <uint32 BE length> <utf8 JSON header>
 *                  <body_bytes of size = h * stride>          (no framing)
 *
 * The JSON header of a frame carries { w, h, stride, ... } so the peer
 * knows exactly how many body bytes follow the header.
 *
 * One instance per DeviceAgent (per-camera). All I/O is blocking.
 * Failure policy: any I/O error closes the fd; next call re-tries connect
 * with a 2 s cooldown.
 */
class IpcClient
{
public:
    IpcClient(std::string socketPath, std::string cameraUuid);
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    /// Send {"type":"attach", "camera_uuid":...}. No reply.
    bool sendAttach();

    /// Send {"type":"detach", "camera_uuid":...}. No reply. Also closes fd.
    void sendDetach();

    /// Send {"type":"config", ...}. No reply. `extra` is merged into the
    /// outgoing object on top of {type, camera_uuid}.
    bool sendConfig(const nx::kit::Json::object& extra);

    /// Send a frame (JSON header + raw body) and wait for a
    /// `{"type":"result", ...}` reply. Returns parsed JSON on success,
    /// `Json()` on failure.
    nx::kit::Json sendFrameAndRecv(
        int64_t frameId, int64_t tsUs,
        int w, int h, int stride,
        const uint8_t* body, size_t bodySize);

private:
    bool ensureConnected();
    void disconnect();
    bool writeFramed(const std::string& utf8);
    bool writeRaw(const void* buf, size_t n);
    bool readFramed(std::string* out);
    bool sendJsonNoReply(const nx::kit::Json& obj);

    const std::string m_socketPath;
    const std::string m_cameraUuid;

    int m_fd = -1;
    std::chrono::steady_clock::time_point m_nextConnectAttempt{};
    int m_consecutiveFailures = 0;
};

}}}} // namespace
