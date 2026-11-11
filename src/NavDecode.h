#pragma once
#include <cstdint>
#include <string>
// Human-readable one-liner for a navigation-channel message, using the protobuf definitions we build against (best effort: the ids/layouts in newer Android Auto
// builds may differ; the raw bytes are always logged next to it).
std::string explainNavMessage(uint16_t id, const std::string& body);
