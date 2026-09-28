// Independent DSP tests; no After Effects host, SDK, files, or devices required.
#include "../src/AudioEnvelope.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
int checks = 0;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
void near(float a, float b, float tolerance, const char* message) {
    require(std::abs(a - b) <= tolerance, message);
}
template<class F> void invalid(F call, const char* message) {
    bool rejected = false;
    try { call(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, message);
}
std::vector<std::int16_t> tone(double rate, double duration, double peak,
    int channels = 1, bool opposite = false, double voicedUntil = 100.0,
    double voicedFrom = 0.0) {
    const auto frames = static_cast<std::size_t>(std::llround(rate * duration));
    std::vector<std::int16_t> result(frames * static_cast<std::size_t>(channels));
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t i = 0; i < frames; ++i) {
        const double time = static_cast<double>(i) / rate;
        const double value = time >= voicedFrom && time < voicedUntil ? peak * std::sin(2.0 * pi * 200.0 * time) : 0.0;
        const auto sample = static_cast<std::int16_t>(std::llround(value));
        for (int channel = 0; channel < channels; ++channel)
            result[i * static_cast<std::size_t>(channels) + channel] = channel && opposite ? -sample : sample;
    }
    return result;
}
float analyze(const std::vector<std::int16_t>& data, double rate = l2dae::kLipAudioSampleRate,
    int channels = 1, double gain = 1.0) {
    return l2dae::analyzeMouthOpen(data.data(), data.size() / static_cast<std::size_t>(channels), channels, rate, gain);
}
}

int main() {
    try {
        using l2dae::analyzeMouthOpen;
        constexpr double rate = l2dae::kLipAudioSampleRate;
        constexpr double window = l2dae::kLipAudioWindowSeconds;
        const auto frames = static_cast<std::size_t>(std::llround(rate * window));
        require(analyzeMouthOpen(nullptr, 0, 1, rate) == 0.0f, "Empty audio is not silence.");
        std::vector<std::int16_t> silence(frames * 2);
        require(analyze(silence, rate, 2) == 0.0f, "Silence opens the mouth.");
        std::vector<std::int16_t> noise(frames);
        std::uint32_t random = 0x72ab391du;
        for (auto& sample : noise) {
            random = random * 1664525u + 1013904223u;
            sample = static_cast<std::int16_t>(static_cast<int>(random % 201) - 100);
        }
        require(analyze(noise) == 0.0f, "Low background noise opens the mouth.");
        auto speech = tone(rate, window, 9000);
        const float normal = analyze(speech);
        require(normal > 0.8f && normal <= 1.0f, "Normal speech should visibly open the mouth.");
        require(analyze(speech, rate, 1, 0.0) == 0.0f, "Zero sensitivity does not close the mouth.");
        require(analyze(speech, rate, 1, std::numeric_limits<double>::max()) <= 1.0f,
            "Huge finite sensitivity did not saturate safely.");

        float previous = 0.0f;
        for (double amplitude : {100.0, 200.0, 400.0, 1000.0, 2500.0, 5000.0, 10000.0, 20000.0}) {
            const float level = analyze(tone(rate, window, amplitude));
            require(level >= previous && level >= 0.0f && level <= 1.0f, "Amplitude response is not monotonic/bounded.");
            previous = level;
        }
        previous = 0.0f;
        for (double gain : {0.0, 0.1, 0.25, 0.5, 1.0, 2.0, 10.0}) {
            const float level = analyze(speech, rate, 1, gain);
            require(level >= previous && level <= 1.0f, "Sensitivity response is not monotonic/bounded.");
            previous = level;
        }

        const auto identical = tone(rate, window, 9000, 2);
        const auto antiphase = tone(rate, window, 9000, 2, true);
        near(analyze(identical, rate, 2), normal, 1e-6f, "Identical stereo disagrees with mono.");
        near(analyze(antiphase, rate, 2), normal, 1e-6f, "Opposite-polarity stereo cancels.");
        auto swapped = antiphase;
        for (std::size_t i = 0; i < swapped.size(); i += 2) std::swap(swapped[i], swapped[i + 1]);
        near(analyze(swapped, rate, 2), normal, 1e-6f, "Stereo channel ordering changes the envelope.");
        auto oneSided = identical;
        for (std::size_t i = 1; i < oneSided.size(); i += 2) oneSided[i] = 0;
        require(analyze(oneSided, rate, 2) > 0.6f, "Speech on one stereo channel is lost.");

        // A new syllable rises quickly, without opening instantly at full strength.
        const float shortPulse = analyze(tone(rate, window, 14000, 1, false, window, window - .01));
        require(shortPulse > .25f && shortPulse < .75f, "Ten-millisecond pulse has incorrect attack behavior.");
        const float sustainedPulse = analyze(tone(rate, window, 14000, 1, false, window, window - .04));
        require(sustainedPulse > shortPulse && sustainedPulse > .85f, "Sustained syllable does not attack promptly.");
        const float full = analyze(tone(rate, window, 14000));
        const float release20 = analyze(tone(rate, window, 14000, 1, false, window - .02));
        const float release50 = analyze(tone(rate, window, 14000, 1, false, window - .05));
        const float release100 = analyze(tone(rate, window, 14000, 1, false, window - .10));
        require(full > release20 && release20 > release50 && release50 > release100 && release100 > 0.0f,
            "Silence tail should close smoothly and monotonically.");
        require(release20 > .5f && release20 < .85f, "Twenty-millisecond release is too fast or too slow.");
        require(release50 > .25f && release50 < .5f, "Fifty-millisecond release is inconsistent.");
        require(release100 < .2f, "Long silence tail keeps the mouth open.");
        require(analyze(tone(rate, window, 14000, 1, false, 0.0)) == 0.0f, "A fully silent window retains past state.");

        // Time constants should describe seconds, not a fixed sample count.
        for (double otherRate : {8000.0, 16000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
            near(analyze(tone(otherRate, window, 9000), otherRate), normal, .002f, "Sample rate changes sustained envelope.");
            near(analyze(tone(otherRate, window, 14000, 1, false, window - .05), otherRate), release50, .015f,
                "Sample rate changes the release time.");
        }
        // Reordering calls, switching channels/gain and large signal changes do not leak history.
        for (int i = 0; i < 12; ++i) {
            analyze(noise); analyze(antiphase, rate, 2, (i + 1) * .3);
            analyze(tone(48000, window, 20000, 1, false, .02), 48000);
            require(analyze(speech) == normal, "Random access depends on previous calls.");
        }
        const std::int16_t maximum = 32767, minimum = -32768;
        const float onePositive = analyzeMouthOpen(&maximum, 1, 1, rate);
        const float oneNegative = analyzeMouthOpen(&minimum, 1, 1, rate);
        require(onePositive > 0 && onePositive < .01f && oneNegative > 0 && oneNegative < .01f,
            "Single-sample boundary should produce a tiny finite attack.");
        std::vector<std::int16_t> padded(frames + 1, 0);
        require(analyze(padded) == 0.0f, "One extra SDK zero frame is not supported.");

        invalid([&] { analyzeMouthOpen(nullptr, 1, 1, rate); }, "Nonempty null buffer was accepted.");
        invalid([&] { analyzeMouthOpen(nullptr, 1, 1, rate, 0); }, "Zero gain bypasses invalid buffer validation.");
        for (int channels : {0, -1, 3, 100})
            invalid([&] { analyzeMouthOpen(&maximum, 1, channels, rate); }, "Unsupported channel count accepted.");
        for (double badRate : {0.0, -1.0, 999.0, 384001.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
            invalid([&] { analyzeMouthOpen(&maximum, 1, 1, badRate); }, "Invalid sample rate accepted.");
        for (double gain : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
            invalid([&] { analyzeMouthOpen(&maximum, 1, 1, rate, gain); }, "Invalid sensitivity accepted.");
        invalid([&] { analyzeMouthOpen(&maximum, frames + 2, 1, rate); }, "Oversized audio window accepted.");
        invalid([&] { analyzeMouthOpen(&maximum, std::numeric_limits<std::size_t>::max(), 2, rate); }, "Overflowing sample count accepted.");
        invalid([&] { analyzeMouthOpen(nullptr, 0, 0, rate); }, "Empty input bypasses invalid format validation.");

        std::cout << "PASS: " << checks << " audio envelope checks; silence/noise, gain, stereo polarity, attack/release, sample rates, seek independence and validation.\n";
        std::cout << "Measured: normal=" << normal << ", pulse10ms=" << shortPulse << ", release20/50/100ms="
            << release20 << '/' << release50 << '/' << release100 << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
