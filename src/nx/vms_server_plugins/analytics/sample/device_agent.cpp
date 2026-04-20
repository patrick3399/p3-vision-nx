// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0.

#include "device_agent.h"
#include "engine.h"
#include "coco80.h"

#include <cstdlib>
#include <map>
#include <sstream>
#include <string>

#include <nx/kit/debug.h>
#include <nx/kit/json.h>
#include <nx/sdk/i_string_map.h>
#include <nx/sdk/helpers/active_setting_changed_response.h>
#include <nx/sdk/helpers/settings_response.h>
#include <nx/sdk/helpers/uuid_helper.h>
#include <nx/sdk/analytics/helpers/object_metadata.h>
#include <nx/sdk/analytics/helpers/object_metadata_packet.h>
#include <nx/sdk/analytics/rect.h>

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

using namespace nx::sdk;
using namespace nx::sdk::analytics;

namespace {

constexpr const char* kSocketPath = "/run/p3-vision-nx/worker_default.sock";

float parseFloatPercent(const std::string& s, float fallback)
{
    if (s.empty()) return fallback;
    try { return std::stof(s) / 100.0f; }
    catch (...) { return fallback; }
}

int parseInt(const std::string& s, int fallback)
{
    if (s.empty()) return fallback;
    try { return std::stoi(s); }
    catch (...) { return fallback; }
}

/**
 * Parse the raw setting value emitted by a CheckBoxGroup.
 *
 * NX Desktop Client round-trips the group as a stringified JSON array of
 * strings (same wire shape as PolygonFigure — the value is a JSON string,
 * not a nested object). Any parse failure is treated as "setting was never
 * populated" (→ outList
 * cleared, outHasValue=false) so the caller skips sending a `classes` key
 * and the worker falls back to "emit all classes".
 *
 * An empty array ("[]") is preserved as "set, but to nothing" — the worker
 * short-circuits inference in that state.
 */
bool parseClassesJson(
    const std::string& raw,
    std::vector<std::string>* outList)
{
    outList->clear();
    if (raw.empty())
        return false;

    std::string err;
    auto parsed = nx::kit::Json::parse(raw, err);
    if (!err.empty() || !parsed.is_array())
        return false;

    for (const auto& item : parsed.array_items())
    {
        if (item.is_string())
            outList->push_back(item.string_value());
    }
    return true;
}

/**
 * Parse a PolygonFigure setting value into a list of (x, y) points.
 *
 * The raw value is a stringified JSON wrapper of shape
 * `{"figure":{"points":[[x,y],[x,y],...]}}` with coordinates already
 * normalized to 0-1. We tolerate two extra shapes that the Desktop Client
 * has emitted historically:
 *   - A top-level object with `points` (no `figure` wrapper)
 *   - A bare `[[x,y],...]` array (stored value after some editions)
 * All failures return false with an empty outPts so the caller can treat
 * it as "polygon not drawn" and skip sending the key over IPC.
 */
bool parsePolygonJson(
    const std::string& raw,
    std::vector<std::pair<float, float>>* outPts)
{
    outPts->clear();
    if (raw.empty())
        return false;

    std::string err;
    auto parsed = nx::kit::Json::parse(raw, err);
    if (!err.empty())
        return false;

    // Locate the point array: parsed.figure.points  OR  parsed.points  OR  parsed itself.
    const nx::kit::Json* pointArr = nullptr;
    if (parsed.is_object())
    {
        const auto& figure = parsed["figure"];
        if (figure.is_object() && figure["points"].is_array())
            pointArr = &figure["points"];
        else if (parsed["points"].is_array())
            pointArr = &parsed["points"];
    }
    else if (parsed.is_array())
    {
        pointArr = &parsed;
    }

    if (!pointArr || !pointArr->is_array())
        return false;

    for (const auto& pt : pointArr->array_items())
    {
        if (!pt.is_array() || pt.array_items().size() < 2)
            continue;
        const float x = static_cast<float>(pt[0].number_value());
        const float y = static_cast<float>(pt[1].number_value());
        outPts->emplace_back(x, y);
    }
    // A polygon with < 3 vertices cannot enclose an area; drop it.
    if (outPts->size() < 3)
    {
        outPts->clear();
        return false;
    }
    return true;
}

/**
 * Parse a BoxFigure setting value into a 4-point CCW polygon.
 *
 * NX's BoxFigure wire format is identical to PolygonFigure
 * (`{"figure":{"points":[[x1,y1],[x2,y2]]}}`) but always contains exactly
 * two opposing corners. We normalize to min/max corners first (Desktop
 * Client lets the user drag either direction, so points[0] may be the
 * bottom-right) and emit a 4-point CCW rectangle so the worker's existing
 * PolygonTest-based filter works without any special case.
 *
 * Failures (empty, < 2 points, degenerate zero-area) clear outPts and
 * return false → caller treats as "ROI not drawn".
 */
bool parseBoxFigure(
    const std::string& raw,
    std::vector<std::pair<float, float>>* outPts)
{
    outPts->clear();
    if (raw.empty())
        return false;

    std::string err;
    auto parsed = nx::kit::Json::parse(raw, err);
    if (!err.empty())
        return false;

    const nx::kit::Json* pointArr = nullptr;
    if (parsed.is_object())
    {
        const auto& figure = parsed["figure"];
        if (figure.is_object() && figure["points"].is_array())
            pointArr = &figure["points"];
        else if (parsed["points"].is_array())
            pointArr = &parsed["points"];
    }
    else if (parsed.is_array())
    {
        pointArr = &parsed;
    }
    if (!pointArr || !pointArr->is_array() || pointArr->array_items().size() < 2)
        return false;

    const auto& a = pointArr->array_items()[0];
    const auto& b = pointArr->array_items()[1];
    if (!a.is_array() || a.array_items().size() < 2
        || !b.is_array() || b.array_items().size() < 2)
    {
        return false;
    }
    const float ax = static_cast<float>(a[0].number_value());
    const float ay = static_cast<float>(a[1].number_value());
    const float bx = static_cast<float>(b[0].number_value());
    const float by = static_cast<float>(b[1].number_value());

    const float x1 = std::min(ax, bx), x2 = std::max(ax, bx);
    const float y1 = std::min(ay, by), y2 = std::max(ay, by);
    // Reject zero-area boxes (user clicked without dragging). Tiny floats are
    // fine — 1 px at 1920×1080 is 5e-4 in normalized units.
    if ((x2 - x1) < 1e-5f || (y2 - y1) < 1e-5f)
        return false;

    // CCW starting top-left: (x1,y1) → (x2,y1) → (x2,y2) → (x1,y2).
    outPts->emplace_back(x1, y1);
    outPts->emplace_back(x2, y1);
    outPts->emplace_back(x2, y2);
    outPts->emplace_back(x1, y2);
    return true;
}

/**
 * Convert an (x, y) polygon vector to the nx::kit::Json array-of-arrays
 * shape that the worker consumes: [[x,y], [x,y], ...].
 */
nx::kit::Json::array polygonToJsonArray(
    const std::vector<std::pair<float, float>>& pts)
{
    nx::kit::Json::array outer;
    outer.reserve(pts.size());
    for (const auto& p : pts)
    {
        nx::kit::Json::array pair;
        pair.emplace_back(p.first);
        pair.emplace_back(p.second);
        outer.emplace_back(std::move(pair));
    }
    return outer;
}

/**
 * Map a COCO-80 class id to the plugin-scoped object type id.
 *
 * Every COCO class is declared as a plugin-scoped type in the Engine
 * manifest's typeLibrary (see coco80.h / engine.cpp), so each detection
 * carries its own human-readable label ("fire hydrant", "laptop", ...)
 * while still inheriting icon + bounding-box colour from the most
 * appropriate `nx.base.*` parent.
 *
 * The table `kCoco80[]` is the single source of truth shared between:
 *   - Engine manifest      → typeLibrary.objectTypes (80 definitions)
 *   - Engine manifest      → Classes CheckBoxGroup range (80 names)
 *   - DeviceAgent manifest → supportedTypes (80 whitelist entries)
 *   - this function        → cls index → typeId
 *
 * Defensive fallback: if the worker hands us a cls outside [0, 80) — e.g.
 * because the loaded model has more classes than COCO, or a crafted IPC
 * reply — we return `nx.base.Unknown` (a built-in that is also declared in
 * supportedTypes) rather than synthesising an undeclared id; the latter
 * would be silently dropped by Desktop Client.
 *
 * Ref: third_party/metadata_sdk/src/nx/sdk/analytics/taxonomy_base_type_library.json
 */
const char* classIdToNxTypeId(int cls)
{
    if (cls < 0 || cls >= kCocoCount)
        return "nx.base.Unknown";
    return kCoco80[cls].typeId;
}

} // namespace

DeviceAgent::DeviceAgent(const nx::sdk::IDeviceInfo* deviceInfo, Engine* engine):
    ConsumingDeviceAgent(deviceInfo, /*enableOutput*/ true),
    m_engine(engine)
{
    m_cameraUuid = (deviceInfo && deviceInfo->id()) ? deviceInfo->id() : "";
    NX_PRINT << "[NXCUSTOM] DeviceAgent ctor cam=" << m_cameraUuid;

    m_ipc = std::make_unique<IpcClient>(kSocketPath, m_cameraUuid);
    m_ipc->sendAttach();

    if (m_engine)
        m_engine->registerAgent(this);
}

DeviceAgent::~DeviceAgent()
{
    if (m_engine)
        m_engine->unregisterAgent(this);
    if (m_ipc)
        m_ipc->sendDetach();
    NX_PRINT << "[NXCUSTOM] DeviceAgent dtor cam=" << m_cameraUuid
             << " frames=" << m_frameCount << " emitted=" << m_emittedPackets;
}

std::string DeviceAgent::manifestString() const
{
    // supportedTypes is the whitelist of typeIds this DeviceAgent may emit.
    // Every entry in kCoco80[] goes here (80 plugin-scoped ids) plus a
    // trailing nx.base.Unknown so the defensive fallback in
    // classIdToNxTypeId() for out-of-range cls still produces a packet
    // Desktop Client accepts instead of silently dropping the overlay.
    //
    // The type DEFINITIONS (id / name / base) live in Engine manifest's
    // typeLibrary.objectTypes — declared once at Engine scope and
    // shared across all cameras under this Engine. Per SDK convention
    // (manifests.md:185) that's where shared plugin types belong.
    std::ostringstream os;
    os << "{\n"
       << "    \"supportedTypes\": " << supportedObjectTypesJson() << ",\n"
       << "    \"typeLibrary\": { \"eventTypes\": [], \"objectTypes\": [] }\n"
       << "}\n";
    return os.str();
}

void DeviceAgent::sendConfigToWorker()
{
    if (!m_ipc)
        return;

    // Snapshot everything under the mutex, then release before touching IPC.
    std::string modelPath, runtime, device;
    float conf, iou;
    int frameSkip;
    std::vector<std::string> classes;
    bool classesSet;
    std::vector<std::pair<float, float>> roi, inclusiveMask, exclusiveMask;
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        modelPath = m_modelPath;
        conf = m_conf;
        iou = m_iou;
        frameSkip = m_frameSkip;
        runtime = m_runtime;
        device  = m_device;
        classes = m_classes;
        classesSet = m_classesSet;
        roi = m_roi;
        inclusiveMask = m_inclusiveMask;
        exclusiveMask = m_exclusiveMask;
    }

    // Fall back to Engine-level defaults (probed from the installed runtimes)
    // if the user has not yet saved per-camera values. This keeps the plugin
    // usable even on first start before any Apply click in Desktop Client.
    int workerCount = 1;
    if (m_engine)
    {
        if (runtime.empty()) runtime = m_engine->runtime();
        if (device.empty())  device  = m_engine->device();
        workerCount = m_engine->workerCount();
    }
    if (runtime.empty()) runtime = "onnx";
    if (device.empty())  device  = "cpu";

    nx::kit::Json::object extra;
    extra["model_path"] = modelPath;
    extra["conf"] = conf;
    extra["iou"] = iou;
    extra["frame_skip"] = frameSkip;
    extra["runtime"] = runtime;
    extra["device"] = device;
    extra["worker_count"] = workerCount;

    // `classes` is a tri-state on the wire:
    //   absent      — user never touched the CheckBoxGroup (or parse failed)
    //                 → worker emits all classes
    //   []          — user explicitly unchecked everything
    //                 → worker short-circuits the model, emits nothing
    //   ["...",...] — user-ticked COCO-80 names, filtered through the model
    if (classesSet)
    {
        nx::kit::Json::array arr;
        arr.reserve(classes.size());
        for (const auto& c : classes)
            arr.emplace_back(c);
        extra["classes"] = arr;
    }

    // Only include polygons that were actually drawn (>= 3 points). The worker
    // treats a missing key as "not set" and skips that region; sending an
    // empty array would be ambiguous.
    if (!roi.empty())
        extra["roi"] = polygonToJsonArray(roi);
    if (!inclusiveMask.empty())
        extra["inclusive_mask"] = polygonToJsonArray(inclusiveMask);
    if (!exclusiveMask.empty())
        extra["exclusive_mask"] = polygonToJsonArray(exclusiveMask);

    const bool ok = m_ipc->sendConfig(extra);

    // Build a compact classes summary for the log (first few names + count).
    std::string classesSummary;
    if (!classesSet)
    {
        classesSummary = "(not set)";
    }
    else if (classes.empty())
    {
        classesSummary = "[] (empty)";
    }
    else
    {
        classesSummary = "[";
        const size_t shown = std::min<size_t>(3, classes.size());
        for (size_t i = 0; i < shown; ++i)
        {
            if (i) classesSummary += ",";
            classesSummary += classes[i];
        }
        if (classes.size() > shown)
            classesSummary += ",...";
        classesSummary += "] x" + std::to_string(classes.size());
    }

    NX_PRINT << "[NXCUSTOM] sendConfig cam=" << m_cameraUuid
             << " ok=" << (ok ? 1 : 0)
             << " runtime=" << runtime
             << " device=" << device
             << " model=" << modelPath
             << " conf=" << conf << " iou=" << iou
             << " frameSkip=" << frameSkip
             << " classes=" << classesSummary
             << " roi=" << roi.size()
             << " incl=" << inclusiveMask.size()
             << " excl=" << exclusiveMask.size();
}

void DeviceAgent::onEngineConfigChanged()
{
    NX_PRINT << "[NXCUSTOM] onEngineConfigChanged cam=" << m_cameraUuid;
    sendConfigToWorker();
}

bool DeviceAgent::pushUncompressedVideoFrame(const IUncompressedVideoFrame* videoFrame)
{
    ++m_frameCount;
    if (!m_ipc || !videoFrame)
        return true;

    int frameSkipLocal;
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        frameSkipLocal = m_frameSkip;
    }
    if (frameSkipLocal > 1 && (m_frameCount % frameSkipLocal) != 0)
        return true;

    const int w = videoFrame->width();
    const int h = videoFrame->height();
    const int stride = videoFrame->lineSize(0);
    const int bodySize = videoFrame->dataSize(0);
    const auto* bytes = reinterpret_cast<const uint8_t*>(videoFrame->data(0));
    const int64_t tsUs = videoFrame->timestampUs();

    if (!bytes || w <= 0 || h <= 0 || stride <= 0 || bodySize <= 0)
        return true;

    if (m_frameCount <= 3)
    {
        NX_PRINT << "[NXCUSTOM] first frames cam=" << m_cameraUuid
                 << " w=" << w << " h=" << h << " stride=" << stride
                 << " bodySize=" << bodySize
                 << " pixelFormat=" << static_cast<int>(videoFrame->pixelFormat());
    }

    auto result = m_ipc->sendFrameAndRecv(
        m_frameCount, tsUs, w, h, stride, bytes, static_cast<size_t>(bodySize));
    if (!result.is_object())
        return true;

    const auto& boxes = result["boxes"];
    if (!boxes.is_array() || boxes.array_items().empty())
        return true;

    auto pack = makePtr<ObjectMetadataPacket>();
    pack->setTimestampUs(tsUs);

    int emitted = 0;
    // Accumulate a compact "(cls→typeId) x N" summary for the log so the user
    // can confirm Desktop Client icons live — first 3 packets + every 100th
    // after that, same cadence as the emitted-packet gate below.
    std::map<std::string, int> typeTally;
    // Tracker observability: how many brand-new track ints did we cache this
    // packet? Lets the journal confirm track_id continuity at a glance
    // without pulling full packet dumps.
    int newUuidsThisPacket = 0;
    for (const auto& box : boxes.array_items())
    {
        const auto& xyxy = box["xyxy"];
        if (!xyxy.is_array() || xyxy.array_items().size() != 4)
            continue;

        const float x1 = static_cast<float>(xyxy[0].number_value());
        const float y1 = static_cast<float>(xyxy[1].number_value());
        const float x2 = static_cast<float>(xyxy[2].number_value());
        const float y2 = static_cast<float>(xyxy[3].number_value());

        // Worker reports the COCO class id in box.cls; map to the most
        // specific NX built-in typeId (see classIdToNxTypeId()). Desktop
        // Client picks the overlay glyph from the typeId, so Car/Truck/Bus
        // get their respective icons rather than a generic Vehicle.
        const int clsId = box["cls"].is_number()
            ? static_cast<int>(box["cls"].number_value())
            : 0;
        const char* typeId = classIdToNxTypeId(clsId);
        ++typeTally[std::string(typeId) + "#" + std::to_string(clsId)];

        // Resolve the worker-side int track_id into a stable UUID so Desktop
        // Client sees one continuous track per object. If the worker did not
        // include a valid track_id (IPC mismatch, transient corruption) we
        // fall back to a fresh random UUID per-box — IDs will flicker but
        // overlays still render.
        nx::sdk::Uuid uuid;
        bool uuidResolved = false;
        if (box["track_id"].is_number())
        {
            const int tid = static_cast<int>(box["track_id"].number_value());
            if (tid > 0)
            {
                auto it = m_trackUuids.find(tid);
                if (it == m_trackUuids.end())
                {
                    uuid = UuidHelper::randomUuid();
                    m_trackUuids.emplace(tid, uuid);
                    ++newUuidsThisPacket;
                }
                else
                {
                    uuid = it->second;
                }
                uuidResolved = true;
            }
        }
        if (!uuidResolved)
            uuid = UuidHelper::randomUuid();

        auto obj = makePtr<ObjectMetadata>();
        obj->setTypeId(typeId);
        obj->setTrackId(uuid);

        Rect bbox;
        bbox.x = x1;
        bbox.y = y1;
        bbox.width = x2 - x1;
        bbox.height = y2 - y1;
        obj->setBoundingBox(bbox);

        pack->addItem(obj.get());
        ++emitted;
    }

    if (emitted > 0)
    {
        pushMetadataPacket(pack.releasePtr());
        ++m_emittedPackets;
        if (m_emittedPackets <= 3 || (m_emittedPackets % 100) == 0)
        {
            std::string tally;
            for (const auto& kv : typeTally)
            {
                if (!tally.empty()) tally += ",";
                tally += kv.first + "x" + std::to_string(kv.second);
            }
            NX_PRINT << "[NXCUSTOM] emitted packet #" << m_emittedPackets
                     << " cam=" << m_cameraUuid << " objects=" << emitted
                     << " tally=" << tally
                     << " trackUuids=" << m_trackUuids.size()
                     << " newUuidsThisPacket=" << newUuidsThisPacket;
        }
    }
    return true;
}

void DeviceAgent::doSetNeededMetadataTypes(
    Result<void>* /*outValue*/, const IMetadataTypes* /*neededMetadataTypes*/)
{
}

Result<const ISettingsResponse*> DeviceAgent::settingsReceived()
{
    ++m_settingsCount;

    std::string modelPath = settingValue("modelPath");
    float conf = parseFloatPercent(settingValue("conf"), 0.30f);
    float iou = parseFloatPercent(settingValue("iou"), 0.50f);
    int frameSkip = parseInt(settingValue("frameSkip"), 1);
    std::string runtime = settingValue("runtime");
    std::string device  = settingValue("device");

    // CheckBoxGroup -> stringified JSON array. Empty / invalid string means
    // we won't send a `classes` key at all (worker defaults to "emit all").
    std::string classesRaw = settingValue("classes");
    std::vector<std::string> classesParsed;
    const bool classesSet = parseClassesJson(classesRaw, &classesParsed);

    // Region settings arrive as a stringified JSON wrapper
    // ({"figure":{"points":[...]}}).
    //   - roi is a BoxFigure (exactly 2 corners) → parseBoxFigure expands to
    //     a 4-point CCW rectangle so the worker's polygon-test filter stays
    //     uniform. Aligns with NX AI Manager convention (ROI = rectangle).
    //   - inclusiveMask / exclusiveMask are PolygonFigure (>= 3 points) for
    //     irregular shapes.
    // Empty / unparseable input → empty vector, worker treats that region as
    // "not drawn" (i.e. no-op for that region in the (ROI ∪ Incl) \ Excl test).
    std::string roiRaw = settingValue("roi");
    std::string inclRaw = settingValue("inclusiveMask");
    std::string exclRaw = settingValue("exclusiveMask");
    std::vector<std::pair<float, float>> roiParsed, inclParsed, exclParsed;
    parseBoxFigure(roiRaw, &roiParsed);
    parsePolygonJson(inclRaw, &inclParsed);
    parsePolygonJson(exclRaw, &exclParsed);
    const size_t roiPts = roiParsed.size();
    const size_t inclPts = inclParsed.size();
    const size_t exclPts = exclParsed.size();

    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_modelPath = modelPath;
        m_conf = conf;
        m_iou = iou;
        m_frameSkip = frameSkip;
        m_runtime = runtime;
        m_device = device;
        m_classes = classesParsed;
        m_classesSet = classesSet;
        m_roi = std::move(roiParsed);
        m_inclusiveMask = std::move(inclParsed);
        m_exclusiveMask = std::move(exclParsed);
    }

    NX_PRINT << "[NXCUSTOM] settingsReceived #" << m_settingsCount
             << " cam=" << m_cameraUuid
             << " runtime=" << runtime << " device=" << device
             << " modelPath=" << modelPath
             << " classesRaw='" << classesRaw << "'"
             << " classesParsed=" << classesParsed.size()
             << " classesSet=" << (classesSet ? 1 : 0)
             << " roiPts=" << roiPts
             << " inclPts=" << inclPts
             << " exclPts=" << exclPts;

    sendConfigToWorker();
    return nullptr;
}

void DeviceAgent::doGetSettingsOnActiveSettingChange(
    Result<const IActiveSettingChangedResponse*>* outResult,
    const IActiveSettingChangedAction* action)
{
    const std::string name = (action && action->activeSettingName())
        ? action->activeSettingName() : "";
    NX_PRINT << "[NXCUSTOM] active setting: " << name << " cam=" << m_cameraUuid;

    auto response = makePtr<ActiveSettingChangedResponse>();

    // Select all / Deselect all buttons for the Classes CheckBoxGroup.
    //
    // Flow: user clicks button → server calls doGetSettingsOnActive… with
    // the button's `name`. We reply with a SettingsResponse that overrides
    // the `classes` value; Desktop Client re-renders the CheckBoxGroup with
    // the new ticks. The user still needs to hit Apply for the change to
    // propagate to settingsReceived() → sendConfigToWorker(). This matches
    // NX AI Manager's official behaviour (button = UI-only tick change,
    // Apply = persist + push to worker).
    if (name == "classesSelectAll" || name == "classesDeselectAll")
    {
        const std::string value = (name == "classesSelectAll")
            ? allClassesJsonArray()   // e.g. ["person","bicycle",…,"toothbrush"]
            : std::string("[]");      // empty selection → worker short-circuits
        auto settings = makePtr<SettingsResponse>();
        settings->setValue("classes", value);
        response->setSettingsResponse(settings);

        NX_PRINT << "[NXCUSTOM] classes override via button='" << name
                 << "' newValueLen=" << value.size();
    }

    *outResult = response.releasePtr();
}

}}}} // namespace
