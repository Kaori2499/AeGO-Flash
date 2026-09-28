#pragma once
#include <cstddef>
#include <cstdint>

namespace l2dae {
constexpr double kLipAudioWindowSeconds = 0.16;
constexpr int kLipAudioSampleRate = 22050;

// Analyze a window ending at the requested render time. frames is the number
// of frames per channel, not the total count of interleaved samples. The caller
// supplies signed PCM16 mono/stereo data and owns the buffer throughout this call.
// No state is retained, so seeking/render order cannot change the result.
// Empty input returns zero; a nonempty buffer may not be null. Invalid metadata
// throws std::invalid_argument. At most 160 ms plus one SDK padding frame is
// accepted, at sample rates from 1 kHz through 384 kHz.
float analyzeMouthOpen(const std::int16_t* interleaved, std::size_t frames,
    int channels, double sampleRate, double sensitivity = 1.0);
}
