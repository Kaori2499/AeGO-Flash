#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace l2dae {
struct MotionJsonResult {
    std::vector<unsigned char> bytes;
    std::size_t convertedStepSegments = 0;
};

// Cubism ends a JSON number only at a comma or a newline. Compact exporters
// put '}' or ']' there, which the SDK rejects as a non-numeric character.
// Insert the missing newline outside strings, before any whitespace that
// follows the number.
std::string terminateCubismNumbers(std::string_view text);

// Validates before entering the SDK parser. The returned, in-memory copy uses
// SDK-compatible JSON spelling and converts only the recognized infinite
// incoming-tangent pattern to a standard stepped segment.
MotionJsonResult normalizeMotionJson(const std::vector<unsigned char>& input);
}
