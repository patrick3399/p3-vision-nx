// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0.

#pragma once

// COCO-80 shared class table.
//
// Every COCO class is declared as a plugin-scoped type in the Engine
// manifest's typeLibrary, so each detection carries a human-readable label
// in Desktop Client instead of collapsing into "Unknown #<classId>". Built-in
// parents (`nx.base.Person`, `nx.base.Car`, ...) still supply icon and
// bounding-box colour where a clean taxonomy home exists.
//
// One source of truth:
//   - engine.cpp  : Classes CheckBoxGroup range (display names) +
//                   typeLibrary.objectTypes (definitions w/ id + name + base)
//   - device_agent.cpp : supportedTypes (whitelist) + classIdToNxTypeId()
//
// All three derive from `kCoco80` below. If you add / rename / re-map a class,
// this is the ONLY table you need to touch.

#include <string>

namespace nx { namespace vms_server_plugins { namespace analytics { namespace sample {

struct CocoClass
{
    // Display name, identical to the string that appears in the
    // "Detect classes" CheckBoxGroup. Must match Ultralytics COCO naming
    // (e.g. "motorcycle", "airplane", "couch", "tv", "potted plant") so
    // names_to_ids() in infer_utils.py can resolve the user's ticks.
    const char* name;

    // Plugin-scoped object type id sent to Desktop Client. Naming follows
    // NX taxonomy identifier rules (letters/digits/underscores only, no
    // spaces); multi-word classes use camelCase after the prefix:
    // "p3.vision.fireHydrant", "p3.vision.cellPhone".
    const char* typeId;

    // NX built-in taxonomy parent. Desktop Client inherits icon + bounding
    // box colour from the base type. Classes with a clean taxonomy home
    // (person / vehicles / cat / dog / bird) inherit those; the rest fall
    // back to nx.base.Unknown but still carry a human-readable `name`.
    const char* baseType;
};

inline constexpr int kCocoCount = 80;

// Order and spelling match Ultralytics COCO — do NOT reorder without also
// updating python/infer_utils.py::names_to_ids() class lookups and the
// worker's `cls` integer contract (cls is the index into this array).
inline constexpr CocoClass kCoco80[kCocoCount] = {
    { "person",         "p3.vision.person",         "nx.base.Person" },
    { "bicycle",        "p3.vision.bicycle",        "nx.base.Bike" },
    { "car",            "p3.vision.car",            "nx.base.Car" },
    { "motorcycle",     "p3.vision.motorcycle",     "nx.base.Bike" },
    { "airplane",       "p3.vision.airplane",       "nx.base.AirTransport" },
    { "bus",            "p3.vision.bus",            "nx.base.Bus" },
    { "train",          "p3.vision.train",          "nx.base.Train" },
    { "truck",          "p3.vision.truck",          "nx.base.Truck" },
    { "boat",           "p3.vision.boat",           "nx.base.WaterTransport" },
    { "traffic light",  "p3.vision.trafficLight",   "nx.base.Unknown" },
    { "fire hydrant",   "p3.vision.fireHydrant",    "nx.base.Unknown" },
    { "stop sign",      "p3.vision.stopSign",       "nx.base.Unknown" },
    { "parking meter",  "p3.vision.parkingMeter",   "nx.base.Unknown" },
    { "bench",          "p3.vision.bench",          "nx.base.Unknown" },
    { "bird",           "p3.vision.bird",           "nx.base.Bird" },
    { "cat",            "p3.vision.cat",            "nx.base.Cat" },
    { "dog",            "p3.vision.dog",            "nx.base.Dog" },
    { "horse",          "p3.vision.horse",          "nx.base.Animal" },
    { "sheep",          "p3.vision.sheep",          "nx.base.Animal" },
    { "cow",            "p3.vision.cow",            "nx.base.Animal" },
    { "elephant",       "p3.vision.elephant",       "nx.base.Animal" },
    { "bear",           "p3.vision.bear",           "nx.base.Animal" },
    { "zebra",          "p3.vision.zebra",          "nx.base.Animal" },
    { "giraffe",        "p3.vision.giraffe",        "nx.base.Animal" },
    { "backpack",       "p3.vision.backpack",       "nx.base.Unknown" },
    { "umbrella",       "p3.vision.umbrella",       "nx.base.Unknown" },
    { "handbag",        "p3.vision.handbag",        "nx.base.Unknown" },
    { "tie",            "p3.vision.tie",            "nx.base.Unknown" },
    { "suitcase",       "p3.vision.suitcase",       "nx.base.Unknown" },
    { "frisbee",        "p3.vision.frisbee",        "nx.base.Unknown" },
    { "skis",           "p3.vision.skis",           "nx.base.Unknown" },
    { "snowboard",      "p3.vision.snowboard",      "nx.base.Unknown" },
    { "sports ball",    "p3.vision.sportsBall",     "nx.base.Unknown" },
    { "kite",           "p3.vision.kite",           "nx.base.Unknown" },
    { "baseball bat",   "p3.vision.baseballBat",    "nx.base.Unknown" },
    { "baseball glove", "p3.vision.baseballGlove",  "nx.base.Unknown" },
    { "skateboard",     "p3.vision.skateboard",     "nx.base.Unknown" },
    { "surfboard",      "p3.vision.surfboard",      "nx.base.Unknown" },
    { "tennis racket",  "p3.vision.tennisRacket",   "nx.base.Unknown" },
    { "bottle",         "p3.vision.bottle",         "nx.base.Unknown" },
    { "wine glass",     "p3.vision.wineGlass",      "nx.base.Unknown" },
    { "cup",            "p3.vision.cup",            "nx.base.Unknown" },
    { "fork",           "p3.vision.fork",           "nx.base.Unknown" },
    { "knife",          "p3.vision.knife",          "nx.base.Unknown" },
    { "spoon",          "p3.vision.spoon",          "nx.base.Unknown" },
    { "bowl",           "p3.vision.bowl",           "nx.base.Unknown" },
    { "banana",         "p3.vision.banana",         "nx.base.Unknown" },
    { "apple",          "p3.vision.apple",          "nx.base.Unknown" },
    { "sandwich",       "p3.vision.sandwich",       "nx.base.Unknown" },
    { "orange",         "p3.vision.orange",         "nx.base.Unknown" },
    { "broccoli",       "p3.vision.broccoli",       "nx.base.Unknown" },
    { "carrot",         "p3.vision.carrot",         "nx.base.Unknown" },
    { "hot dog",        "p3.vision.hotDog",         "nx.base.Unknown" },
    { "pizza",          "p3.vision.pizza",          "nx.base.Unknown" },
    { "donut",          "p3.vision.donut",          "nx.base.Unknown" },
    { "cake",           "p3.vision.cake",           "nx.base.Unknown" },
    { "chair",          "p3.vision.chair",          "nx.base.Unknown" },
    { "couch",          "p3.vision.couch",          "nx.base.Unknown" },
    { "potted plant",   "p3.vision.pottedPlant",    "nx.base.Unknown" },
    { "bed",            "p3.vision.bed",            "nx.base.Unknown" },
    { "dining table",   "p3.vision.diningTable",    "nx.base.Unknown" },
    { "toilet",         "p3.vision.toilet",         "nx.base.Unknown" },
    { "tv",             "p3.vision.tv",             "nx.base.Unknown" },
    { "laptop",         "p3.vision.laptop",         "nx.base.Unknown" },
    { "mouse",          "p3.vision.mouse",          "nx.base.Unknown" },
    { "remote",         "p3.vision.remote",         "nx.base.Unknown" },
    { "keyboard",       "p3.vision.keyboard",       "nx.base.Unknown" },
    { "cell phone",     "p3.vision.cellPhone",      "nx.base.Unknown" },
    { "microwave",      "p3.vision.microwave",      "nx.base.Unknown" },
    { "oven",           "p3.vision.oven",           "nx.base.Unknown" },
    { "toaster",        "p3.vision.toaster",        "nx.base.Unknown" },
    { "sink",           "p3.vision.sink",           "nx.base.Unknown" },
    { "refrigerator",   "p3.vision.refrigerator",   "nx.base.Unknown" },
    { "book",           "p3.vision.book",           "nx.base.Unknown" },
    { "clock",          "p3.vision.clock",          "nx.base.Unknown" },
    { "vase",           "p3.vision.vase",           "nx.base.Unknown" },
    { "scissors",       "p3.vision.scissors",       "nx.base.Unknown" },
    { "teddy bear",     "p3.vision.teddyBear",      "nx.base.Unknown" },
    { "hair drier",     "p3.vision.hairDrier",      "nx.base.Unknown" },
    { "toothbrush",     "p3.vision.toothbrush",     "nx.base.Unknown" },
};

// Returns a JSON array literal of all 80 class display names, e.g.
// `["person","bicycle","car",...,"toothbrush"]`. Used as:
//   (a) CheckBoxGroup `range` in the Engine manifest (single source of truth
//       so the list cannot drift from kCoco80).
//   (b) "Select all" button handler response value.
std::string allClassesJsonArray();

// Returns the typeLibrary.objectTypes JSON fragment (array literal) with 80
// `{id,name,base}` entries. Goes into Engine manifest's typeLibrary so
// Desktop Client learns each plugin-scope type's display name + parent.
std::string typeLibraryObjectTypesJson();

// Returns the supportedTypes JSON fragment (array literal) with 80
// `{"objectTypeId":"..."}` entries plus a trailing nx.base.Unknown fallback.
// Goes into DeviceAgent manifest — NX requires emitted typeIds to appear here
// or Desktop Client silently drops the overlay.
std::string supportedObjectTypesJson();

}}}} // namespace
