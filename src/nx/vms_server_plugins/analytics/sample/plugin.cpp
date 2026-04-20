// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0.

#include "plugin.h"
#include "engine.h"

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

using namespace nx::sdk;
using namespace nx::sdk::analytics;

Result<IEngine*> Plugin::doObtainEngine() { return new Engine(); }

std::string Plugin::manifestString() const
{
    return 1 + (const char*) R"json(
{
    "id": "p3.vision",
    "name": "P3 Vision",
    "description": "Self-hosted YOLO analytics plugin. Multi-runtime (ONNX / OpenVINO), COCO-80 class taxonomy, rectangular ROI with pixel-level crop, inclusive / exclusive polygon masks, stable per-object track IDs, Select all / Deselect all shortcuts.",
    "version": "1.0.0",
    "vendor": "Dev By Patrick3399"
}
)json";
}

extern "C" NX_PLUGIN_API nx::sdk::IPlugin* createNxPlugin() { return new Plugin(); }

}}}} // namespace
