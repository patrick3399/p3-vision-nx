// Copyright 2026 Patrick3399. Licensed under MIT (see LICENSE in repo root).

#include "ipc_client.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include <arpa/inet.h> // htonl / ntohl
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nx/kit/debug.h>

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

namespace {

constexpr std::chrono::seconds kConnectCooldown{2};
constexpr int kMaxFrameBytes = 64 * 1024 * 1024;

} // namespace

IpcClient::IpcClient(std::string socketPath, std::string cameraUuid):
    m_socketPath(std::move(socketPath)),
    m_cameraUuid(std::move(cameraUuid))
{
}

IpcClient::~IpcClient()
{
    disconnect();
}

bool IpcClient::ensureConnected()
{
    if (m_fd >= 0)
        return true;

    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextConnectAttempt)
        return false;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        NX_PRINT << "[NXCUSTOM][ipc] socket() failed: " << std::strerror(errno);
        m_nextConnectAttempt = now + kConnectCooldown;
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path))
    {
        NX_PRINT << "[NXCUSTOM][ipc] socket path too long: " << m_socketPath;
        ::close(fd);
        m_nextConnectAttempt = now + kConnectCooldown;
        return false;
    }
    std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        if (m_consecutiveFailures == 0)
        {
            NX_PRINT << "[NXCUSTOM][ipc] connect(" << m_socketPath
                     << ") failed: " << std::strerror(errno)
                     << " (will retry every " << kConnectCooldown.count() << "s, logging suppressed)";
        }
        ++m_consecutiveFailures;
        ::close(fd);
        m_nextConnectAttempt = now + kConnectCooldown;
        return false;
    }

    m_fd = fd;
    m_consecutiveFailures = 0;
    NX_PRINT << "[NXCUSTOM][ipc] connected cam=" << m_cameraUuid
             << " path=" << m_socketPath << " fd=" << fd;
    return true;
}

void IpcClient::disconnect()
{
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool IpcClient::writeRaw(const void* buf, size_t n)
{
    if (m_fd < 0)
        return false;
    const char* p = static_cast<const char*>(buf);
    size_t remaining = n;
    while (remaining > 0)
    {
        ssize_t k = ::send(m_fd, p, remaining, MSG_NOSIGNAL);
        if (k <= 0)
        {
            NX_PRINT << "[NXCUSTOM][ipc] send() failed: " << std::strerror(errno);
            disconnect();
            return false;
        }
        p += k;
        remaining -= k;
    }
    return true;
}

bool IpcClient::writeFramed(const std::string& utf8)
{
    if (m_fd < 0)
        return false;
    if (utf8.size() > static_cast<size_t>(kMaxFrameBytes))
        return false;

    const uint32_t lenBe = htonl(static_cast<uint32_t>(utf8.size()));
    if (!writeRaw(&lenBe, 4))
        return false;
    if (utf8.size() > 0 && !writeRaw(utf8.data(), utf8.size()))
        return false;
    return true;
}

bool IpcClient::readFramed(std::string* out)
{
    if (m_fd < 0 || !out)
        return false;

    uint32_t lenBe = 0;
    char* hp = reinterpret_cast<char*>(&lenBe);
    size_t remaining = 4;
    while (remaining > 0)
    {
        ssize_t n = ::recv(m_fd, hp, remaining, 0);
        if (n <= 0)
        {
            NX_PRINT << "[NXCUSTOM][ipc] recv(hdr) failed: "
                     << (n == 0 ? "peer closed" : std::strerror(errno));
            disconnect();
            return false;
        }
        hp += n;
        remaining -= n;
    }
    const uint32_t length = ntohl(lenBe);
    if (length == 0 || length > static_cast<uint32_t>(kMaxFrameBytes))
    {
        NX_PRINT << "[NXCUSTOM][ipc] bad framed length=" << length;
        disconnect();
        return false;
    }

    out->resize(length);
    char* bp = &(*out)[0];
    remaining = length;
    while (remaining > 0)
    {
        ssize_t n = ::recv(m_fd, bp, remaining, 0);
        if (n <= 0)
        {
            NX_PRINT << "[NXCUSTOM][ipc] recv(body) failed: "
                     << (n == 0 ? "peer closed" : std::strerror(errno));
            disconnect();
            return false;
        }
        bp += n;
        remaining -= n;
    }
    return true;
}

bool IpcClient::sendJsonNoReply(const nx::kit::Json& obj)
{
    if (!ensureConnected())
        return false;
    return writeFramed(obj.dump());
}

bool IpcClient::sendAttach()
{
    nx::kit::Json::object j;
    j["type"] = std::string("attach");
    j["camera_uuid"] = m_cameraUuid;
    return sendJsonNoReply(nx::kit::Json(j));
}

void IpcClient::sendDetach()
{
    if (m_fd >= 0)
    {
        nx::kit::Json::object j;
        j["type"] = std::string("detach");
        j["camera_uuid"] = m_cameraUuid;
        writeFramed(nx::kit::Json(j).dump());
    }
    disconnect();
}

bool IpcClient::sendConfig(const nx::kit::Json::object& extra)
{
    if (!ensureConnected())
        return false;
    nx::kit::Json::object j = extra;
    j["type"] = std::string("config");
    j["camera_uuid"] = m_cameraUuid;
    return writeFramed(nx::kit::Json(j).dump());
}

nx::kit::Json IpcClient::sendFrameAndRecv(
    int64_t frameId, int64_t tsUs,
    int w, int h, int stride,
    const uint8_t* body, size_t bodySize)
{
    if (!ensureConnected())
        return nx::kit::Json();

    nx::kit::Json::object req;
    req["type"] = std::string("frame");
    req["camera_uuid"] = m_cameraUuid;
    req["id"] = static_cast<double>(frameId);
    req["w"] = w;
    req["h"] = h;
    req["stride"] = stride;
    req["ts_us"] = static_cast<double>(tsUs);

    if (!writeFramed(nx::kit::Json(req).dump()))
        return nx::kit::Json();

    if (body && bodySize > 0)
    {
        if (!writeRaw(body, bodySize))
            return nx::kit::Json();
    }

    std::string reply;
    if (!readFramed(&reply))
        return nx::kit::Json();

    std::string err;
    auto parsed = nx::kit::Json::parse(reply, err);
    if (!err.empty())
    {
        NX_PRINT << "[NXCUSTOM][ipc] bad reply JSON: " << err;
        return nx::kit::Json();
    }
    return parsed;
}

}}}} // namespace
