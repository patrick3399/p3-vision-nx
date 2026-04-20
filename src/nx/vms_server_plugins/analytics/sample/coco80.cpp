// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0.

#include "coco80.h"

#include <sstream>

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

// NB: none of these build functions are on the per-frame hot path; they are
// called exactly once per manifest build (Engine/DeviceAgent startup). No
// need to cache / memoise.

std::string allClassesJsonArray()
{
    std::ostringstream os;
    os << "[";
    for (int i = 0; i < kCocoCount; ++i)
    {
        if (i) os << ",";
        os << "\"" << kCoco80[i].name << "\"";
    }
    os << "]";
    return os.str();
}

std::string typeLibraryObjectTypesJson()
{
    std::ostringstream os;
    os << "[";
    for (int i = 0; i < kCocoCount; ++i)
    {
        if (i) os << ",";
        // "name" carries spaces as-is (taxonomy.md allows spaces inside names,
        // banning only leading/trailing/consecutive). "id" is always the
        // camelCase plugin-scoped form.
        os << "{\"id\":\"" << kCoco80[i].typeId
           << "\",\"name\":\"" << kCoco80[i].name
           << "\",\"base\":\"" << kCoco80[i].baseType << "\"}";
    }
    os << "]";
    return os.str();
}

std::string supportedObjectTypesJson()
{
    std::ostringstream os;
    os << "[";
    for (int i = 0; i < kCocoCount; ++i)
    {
        if (i) os << ",";
        os << "{\"objectTypeId\":\"" << kCoco80[i].typeId << "\"}";
    }
    // Defensive: if the worker ever hands us a cls outside [0,79]
    // (mismatched model, out-of-range filter) classIdToNxTypeId() falls back
    // to "nx.base.Unknown" — declare it here so Desktop Client accepts that
    // fallback packet without dropping the whole overlay.
    os << ",{\"objectTypeId\":\"nx.base.Unknown\"}";
    os << "]";
    return os.str();
}

}}}} // namespace
