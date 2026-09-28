#include "AudioEnvelope.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace l2dae {
namespace {
constexpr double kGateDb = -45.0;
constexpr double kFullOpenDb = -12.0;
constexpr double kAttackSeconds = 0.015;
constexpr double kReleaseSeconds = 0.050;
constexpr double kBlockSeconds = 0.010;
constexpr double kPcmScale = 1.0 / 32768.0;
}

float analyzeMouthOpen(const std::int16_t* interleaved, std::size_t frames,
    int channels, double sampleRate, double sensitivity) {
    if ((channels != 1 && channels != 2) || !std::isfinite(sampleRate) ||
        sampleRate < 1000.0 || sampleRate > 384000.0 ||
        !std::isfinite(sensitivity) || sensitivity < 0.0) {
        throw std::invalid_argument("Invalid lip-sync audio format or sensitivity.");
    }
    // The SDK may provide one extra zero frame. A bounded window also prevents
    // overflow in sample indexing or unexpectedly expensive render-time analysis.
    const auto maximumFrames = static_cast<std::size_t>(std::ceil(sampleRate * kLipAudioWindowSeconds)) + 1;
    if (frames > maximumFrames || (frames != 0 && interleaved == nullptr)) {
        throw std::invalid_argument("Invalid lip-sync audio buffer or window length.");
    }
    if (frames == 0 || sensitivity == 0.0) return 0.0f;

    const auto blockFrames = static_cast<std::size_t>(std::llround(sampleRate * kBlockSeconds));
    const double gate = std::pow(10.0, kGateDb / 20.0);
    const double full = std::pow(10.0, kFullOpenDb / 20.0);
    double envelope = 0.0;
    std::size_t offset = 0;
    // Right-align the blocks to the render time. An incomplete block belongs at
    // the start, leaving the newest blocks at a consistent temporal resolution.
    std::size_t count = frames % blockFrames;
    if (count == 0) count = blockFrames;
    while (offset < frames) {
        double energy = 0.0;
        for (std::size_t frame = offset; frame < offset + count; ++frame) {
            for (int channel = 0; channel < channels; ++channel) {
                const double sample = static_cast<double>(interleaved[frame * static_cast<std::size_t>(channels) + channel]) * kPcmScale;
                energy += sample * sample;
            }
        }
        // Combine channel energy instead of summing samples: opposite-polarity
        // stereo speech remains audible to the analyzer instead of cancelling.
        const double rms = std::sqrt(energy / (static_cast<double>(count) * channels));
        double target = 0.0;
        if (rms > 0.0) {
            // Compare before multiplying so even an enormous finite sensitivity
            // saturates safely without overflowing into infinity.
            if (sensitivity >= full / rms) target = 1.0;
            else {
                const double amplified = rms * sensitivity;
                if (amplified > gate) {
                    target = (20.0 * std::log10(amplified) - kGateDb) / (kFullOpenDb - kGateDb);
                }
            }
        }
        const double duration = static_cast<double>(count) / sampleRate;
        const double tau = target > envelope ? kAttackSeconds : kReleaseSeconds;
        const double mix = -std::expm1(-duration / tau);
        envelope += (target - envelope) * mix;
        offset += count;
        count = std::min(blockFrames, frames - offset);
    }
    return static_cast<float>(std::clamp(envelope, 0.0, 1.0));
}
}
