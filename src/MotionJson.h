#pragma once

#include <cstddef>
#include <vector>

namespace l2dae {
struct MotionJsonResult {
    std::vector<unsigned char> bytes;
    std::size_t convertedStepSegments = 0;
};

// Validates before entering the SDK parser. The returned, in-memory copy uses
// SDK-compatible JSON spelling and converts only the recognized infinite
// incoming-tangent pattern to a standard stepped segment.
MotionJsonResult normalizeMotionJson(const std::vector<unsigned char>& input);
}
