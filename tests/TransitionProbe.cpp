// Optional real-model transition reproducer. Input assets are never modified.
// Single-motion frames aid visual diagnosis; intermediate blended geometry has
// no independent visual oracle, so frame-difference metrics are diagnostic only.
#include "Renderer.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void check(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }

std::size_t visiblePixels(const l2dae::RenderResult& frame) {
    require(frame.width > 0 && frame.height > 0 &&
        frame.rgba.size() == static_cast<std::size_t>(frame.width) * frame.height * 4,
        "Renderer returned an invalid pixel buffer.");
    std::size_t count = 0;
    for (std::size_t i = 3; i < frame.rgba.size(); i += 4)
        if (frame.rgba[i] > 16) ++count;
    return count;
}

void samePixels(const l2dae::RenderResult& actual, const l2dae::RenderResult& expected,
    const char* message) {
    require(actual.width == expected.width && actual.height == expected.height &&
        actual.rgba == expected.rgba, message);
}

void describeDifference(const l2dae::RenderRequest& request,
    const l2dae::RenderResult& expected, const l2dae::RenderResult& actual) {
    std::cout << "REVISIT_DIFF motion=" << std::filesystem::path(request.motionPath).filename().u8string()
        << " t=" << std::setprecision(17) << request.seconds
        << " expected_size=" << expected.width << 'x' << expected.height
        << " actual_size=" << actual.width << 'x' << actual.height;
    if (expected.rgba.size() != actual.rgba.size()) {
        std::cout << " expected_bytes=" << expected.rgba.size() << " actual_bytes=" << actual.rgba.size() << '\n';
        return;
    }
    std::size_t pixels = 0;
    std::size_t channels = 0;
    std::size_t alphaPixels = 0;
    std::size_t first = expected.rgba.size();
    unsigned maximum = 0;
    int left = expected.width, top = expected.height, right = -1, bottom = -1;
    for (std::size_t i = 0; i < actual.rgba.size(); i += 4) {
        bool changed = false;
        for (std::size_t c = 0; c < 4; ++c) {
            const unsigned difference = static_cast<unsigned>(std::abs(
                static_cast<int>(actual.rgba[i + c]) - static_cast<int>(expected.rgba[i + c])));
            if (difference) { ++channels; changed = true; }
            maximum = std::max(maximum, difference);
        }
        if (actual.rgba[i + 3] != expected.rgba[i + 3]) ++alphaPixels;
        if (changed) {
            ++pixels;
            if (first == expected.rgba.size()) first = i;
            const int x = static_cast<int>((i / 4) % expected.width);
            const int y = static_cast<int>((i / 4) / expected.width);
            left = std::min(left, x); right = std::max(right, x);
            top = std::min(top, y); bottom = std::max(bottom, y);
        }
    }
    std::cout << " different_pixels=" << pixels << " different_channels=" << channels
        << " different_alpha_pixels=" << alphaPixels << " max_channel_delta=" << maximum
        << " bounds=" << left << ',' << top << ',' << right << ',' << bottom;
    if (first != expected.rgba.size()) {
        std::cout << " first_xy=" << (first / 4) % expected.width << ',' << (first / 4) / expected.width
            << " first_expected_rgba=";
        for (std::size_t c = 0; c < 4; ++c) std::cout << (c ? "," : "") << static_cast<unsigned>(expected.rgba[first + c]);
        std::cout << " first_actual_rgba=";
        for (std::size_t c = 0; c < 4; ++c) std::cout << (c ? "," : "") << static_cast<unsigned>(actual.rgba[first + c]);
    }
    std::cout << '\n';
}

// Same WIC path as RenderProbe: renderer output is premultiplied RGBA, whereas
// PNG receives straight BGRA. Pixel assertions always use original RGBA bytes.
void writePng(const std::filesystem::path& path, const l2dae::RenderResult& frame) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        check(initialized, "COM initialization failed.");
    struct Uninitialize {
        bool active;
        ~Uninitialize() { if (active) CoUninitialize(); }
    } cleanup{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf())), "Cannot create WIC encoder.");
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(stream.GetAddressOf()), "Cannot create output stream.");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Cannot open PNG output.");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()), "Cannot create PNG encoder.");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Cannot initialize PNG encoder.");
    ComPtr<IWICBitmapFrameEncode> encodedFrame;
    ComPtr<IPropertyBag2> options;
    check(encoder->CreateNewFrame(encodedFrame.GetAddressOf(), options.GetAddressOf()), "Cannot create PNG frame.");
    check(encodedFrame->Initialize(options.Get()), "Cannot initialize PNG frame.");
    check(encodedFrame->SetSize(frame.width, frame.height), "Cannot set PNG dimensions.");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(encodedFrame->SetPixelFormat(&format), "Cannot set PNG pixel format.");
    require(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "PNG encoder did not accept BGRA.");
    auto pixels = frame.rgba;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const unsigned alpha = pixels[i + 3];
        for (std::size_t c = 0; c < 3; ++c)
            pixels[i + c] = alpha ? static_cast<std::uint8_t>(std::min(255u,
                (static_cast<unsigned>(pixels[i + c]) * 255u + alpha / 2) / alpha)) : 0;
        std::swap(pixels[i], pixels[i + 2]);
    }
    check(encodedFrame->WritePixels(frame.height, frame.width * 4,
        static_cast<UINT>(pixels.size()), pixels.data()), "Cannot write PNG pixels.");
    check(encodedFrame->Commit(), "Cannot finish PNG frame.");
    check(encoder->Commit(), "Cannot finish PNG file.");
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned ready = 0;
    bool open = false;
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready;
        changed.notify_all();
        changed.wait(lock, [&] { return open; });
    }
    void start(unsigned count) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return ready == count; });
        open = true;
        changed.notify_all();
    }
};

struct Sample {
    int frameNumber;
    l2dae::RenderRequest request;
    l2dae::RenderResult image;
};

std::wstring frameName(int frame) {
    std::wostringstream name;
    name << L"frame-" << std::setfill(L'0') << std::setw(5) << frame;
    return name.str();
}

struct Difference {
    std::size_t pixels = 0, alphaPixels = 0;
    unsigned maximum = 0;
    std::uint64_t absoluteSum = 0;
};
Difference difference(const l2dae::RenderResult& a, const l2dae::RenderResult& b) {
    require(a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size(),
        "Cannot compare different frame dimensions.");
    Difference result;
    for (std::size_t pixel = 0; pixel < a.rgba.size(); pixel += 4) {
        bool changed = false;
        for (std::size_t c = 0; c < 4; ++c) {
            const auto delta = static_cast<unsigned>(std::abs(
                static_cast<int>(a.rgba[pixel + c]) - static_cast<int>(b.rgba[pixel + c])));
            changed = changed || delta != 0;
            result.maximum = std::max(result.maximum, delta);
            result.absoluteSum += delta;
        }
        if (changed) ++result.pixels;
        if (a.rgba[pixel + 3] != b.rgba[pixel + 3]) ++result.alphaPixels;
    }
    return result;
}

double smoothFifth(double value) {
    if (value <= 0) return 0;
    if (value >= 1) return 1;
    return value * value * value * (value * (6.0 * value - 15.0) + 10.0);
}

double transitionWeight(double progress, int curve) {
    const double u = std::clamp(progress, 0.0, 1.0);
    if (curve == 0 || u <= 0 || u >= 1) return u;
    if (curve == 3) return smoothFifth(u);
    if (curve == 4 || curve == 5) {
        const double amplitude = curve == 4 ? 0.03 : 0.075;
        return u <= 0.6 ? (1.0 + amplitude) * smoothFifth(u / 0.6) :
            1.0 + amplitude * (1.0 - smoothFifth((u - 0.6) / 0.4));
    }
    const double remaining = 1.0 - u;
    if (curve == 2) return 3.0 * u * u - 2.0 * u * u * u +
        7.5 * u * u * u * remaining * remaining;
    const double oscillation = std::cos(2.0 * std::acos(-1.0) * u);
    return 1.0 - remaining * remaining * remaining * oscillation * oscillation;
}

void checkCurveLandmarks() {
    // Independent exact values at eighth-turn angles, shared with the JS
    // acceptance expectations rather than computed with its formula again.
    const std::array<double, 7> progress{0, .125, .25, .5, .75, .875, 1};
    const std::array<double, 7> expected{0, .6650390625, 1, .875, 1, .9990234375, 1};
    for (std::size_t i = 0; i < progress.size(); ++i) {
        require(transitionWeight(progress[i], 0) == progress[i], "Linear transition changed progress.");
        require(std::abs(transitionWeight(progress[i], 1) - expected[i]) <= 1e-12,
            "Q-elastic transition does not match its independent curve landmarks.");
    }
    std::cout << "CURVE_REFERENCE landmarks=" << progress.size() << " passed=" << progress.size() << '\n';
    const std::array<double, 8> elasticProgress{0, .125, .25, .5, .75, .8, .875, 1};
    const std::array<double, 8> elasticExpected{0, .0541839599609375, .22216796875,
        .734375, 1.04150390625, 1.0496, 1.0355377197265625, 1};
    for (std::size_t i = 0; i < elasticProgress.size(); ++i)
        require(std::abs(transitionWeight(elasticProgress[i], 2) - elasticExpected[i]) <= 1e-12,
            "True overshoot transition does not match independent curve landmarks.");
    std::cout << "OVERSHOOT_CURVE_REFERENCE landmarks=" << elasticProgress.size()
        << " passed=" << elasticProgress.size() << " peak_weight=1.0496\n";
    const std::array<double, 7> smoothProgress{0, .1, .25, .5, .75, .9, 1};
    const std::array<double, 7> smoothExpected{0, .00856, .103515625, .5, .896484375, .99144, 1};
    for (std::size_t i = 0; i < smoothProgress.size(); ++i)
        require(std::abs(transitionWeight(smoothProgress[i], 3) - smoothExpected[i]) <= 1e-12,
            "No-overshoot smooth curve differs from its independent landmarks.");
    const std::array<double, 9> selectedProgress{0, .15, .3, .45, .6, .7, .8, .9, 1};
    const std::array<double, 9> gentleExpected{0, .10662109375, .515, .92337890625,
        1.03, 1.02689453125, 1.015, 1.00310546875, 1};
    const std::array<double, 9> clearExpected{0, .111279296875, .5375, .963720703125,
        1.075, 1.067236328125, 1.0375, 1.007763671875, 1};
    for (std::size_t i = 0; i < selectedProgress.size(); ++i) {
        require(std::abs(transitionWeight(selectedProgress[i], 4) - gentleExpected[i]) <= 1e-12,
            "Gentle overshoot differs from its independent landmarks.");
        require(std::abs(transitionWeight(selectedProgress[i], 5) - clearExpected[i]) <= 1e-12,
            "Clear overshoot differs from its independent landmarks.");
    }
    for (int curve = 3; curve <= 5; ++curve) {
        const double peak = curve == 3 ? 1.0 : curve == 4 ? 1.03 : 1.075;
        const double turn = curve == 3 ? 1.0 : 0.6;
        double previous = 0;
        for (int step = 0; step <= 200; ++step) {
            const double value = transitionWeight(turn * step / 200.0, curve);
            require(value >= previous - 1e-14 && value >= 0 && value <= peak + 1e-14,
                "The smooth approach is not bounded and monotone.");
            previous = value;
        }
        if (curve != 3) {
            for (int step = 1; step <= 200; ++step) {
                const double value = transitionWeight(0.6 + 0.4 * step / 200.0, curve);
                require(value <= previous + 1e-14 && value >= 1 && value <= peak + 1e-14,
                    "The extended settling tail is not bounded and monotone.");
                previous = value;
            }
            require(std::abs(transitionWeight(.8, curve) - (1.0 + (peak - 1.0) / 2.0)) < 1e-12,
                "The settling phase no longer occupies the final 40 percent.");
        }
        // A cubic leading error shrinks by eight when the time distance halves.
        // This independently detects zero velocity and acceleration at each
        // endpoint and both sides of the peak, without differentiating the same
        // production polynomial a second time or permitting a corner at the join.
        const auto cubicApproach = [](double farError, double nearError) {
            require(nearError > 0 && farError / nearError > 7.5 && farError / nearError < 8.5,
                "A new curve endpoint or peak does not have smooth velocity and acceleration.");
        };
        const double h = 0.002;
        cubicApproach(transitionWeight(h, curve), transitionWeight(h / 2, curve));
        if (curve == 3) {
            cubicApproach(1.0 - transitionWeight(1.0 - h, curve),
                1.0 - transitionWeight(1.0 - h / 2, curve));
        } else {
            cubicApproach(peak - transitionWeight(.6 - h, curve), peak - transitionWeight(.6 - h / 2, curve));
            cubicApproach(peak - transitionWeight(.6 + h, curve), peak - transitionWeight(.6 + h / 2, curve));
            cubicApproach(transitionWeight(1.0 - h, curve) - 1.0,
                transitionWeight(1.0 - h / 2, curve) - 1.0);
        }
    }
    std::cout << "V160_CURVE_REFERENCE landmarks=25 passed=25 amplitudes=0,0.03,0.075 "
        "settle_fraction=0.4 bounded_monotone=1 endpoint_and_peak_C2=1\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 5 || argc > 8) {
        std::cerr << "Usage: Live2DTransitionProbe model3.json motion-A.json motion-B.json output-folder "
            "[start-B-frame=64 [fps=30 [curve=0 (0=linear,1=bounded-rebound,2=legacy-overshoot,3=none,4=gentle,5=clear)]]]\n";
        return 2;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const auto model = std::filesystem::absolute(argv[1]);
        const auto pathA = std::filesystem::absolute(argv[2]);
        const auto pathB = std::filesystem::absolute(argv[3]);
        const auto output = std::filesystem::absolute(argv[4]);
        require(std::filesystem::is_regular_file(model) && std::filesystem::is_regular_file(pathA)
            && std::filesystem::is_regular_file(pathB), "Model or motion file is missing.");
        const double startFrameB = argc >= 6 ? std::stod(argv[5]) : 64.0;
        const double fps = argc >= 7 ? std::stod(argv[6]) : 30.0;
        require(argc < 8 || (std::wstring(argv[7]).size() == 1 && argv[7][0] >= L'0' && argv[7][0] <= L'5'),
            "Transition curve must be 0 through 5.");
        const int curve = argc >= 8 ? std::stoi(argv[7]) : 0;
        checkCurveLandmarks();
        require(std::isfinite(fps) && fps > 0 && fps <= 1000 && std::isfinite(startFrameB)
            && startFrameB >= 0, "Invalid frame rate or motion B start frame.");
        const double durationA = l2dae::motionDuration(pathA.wstring());
        const double durationB = l2dae::motionDuration(pathB.wstring());
        const double startB = startFrameB / fps;
        const double endA = durationA;
        require(startB < endA && startB + durationB > endA,
            "Probe expects B to overlap A's end and extend beyond it.");
        const int firstFrame = static_cast<int>(std::floor(startFrameB)) - 2;
        const int lastFrame = static_cast<int>(std::ceil(endA * fps)) + 2;
        require(lastFrame - firstFrame <= 1000, "Refusing to produce more than 1001 transition frames.");
        std::filesystem::create_directories(output / L"transition");
        std::filesystem::create_directories(output / L"single-A");
        std::filesystem::create_directories(output / L"single-B");
        std::ofstream manifest(output / L"frames.csv");
        require(manifest.good(), "Cannot create frame manifest.");
        manifest << "frame,time_seconds,source_A,source_B,time_A,time_B,normalized_progress,transition_curve,blend,visible_pixels,"
            "changed_pixels,changed_alpha_pixels,max_channel_delta,total_absolute_channel_delta\n";
        manifest << std::setprecision(17);
        std::cout << std::setprecision(17)
            << "MODEL " << model.filename().u8string() << " width=540 height=960 fps=" << fps << '\n'
            << "TIMELINE A=" << pathA.filename().u8string() << " duration_A=" << durationA
            << " B=" << pathB.filename().u8string() << " duration_B=" << durationB
            << " start_B_frame=" << startFrameB << " start_B_seconds=" << startB
            << " overlap_frames=" << (endA - startB) * fps << " transition_curve=" << curve << '\n';
        std::cout << "SCOPE: Adjacent frame differences are diagnostic only; no jump threshold is asserted. "
            "Use fractional start-B-frame=(duration_A-0.5)*fps to match exact 15-frame append.\n";

        std::vector<Sample> samples;
        std::size_t boundaryComparisons = 0;
        for (int frame = firstFrame; frame <= lastFrame; ++frame) {
            const double time = static_cast<double>(frame) / fps;
            const double timeA = std::clamp(time, 0.0, durationA);
            const double timeB = std::clamp(time - startB, 0.0, durationB);
            l2dae::RenderRequest request;
            request.modelPath = model.wstring();
            request.width = 540;
            request.height = 960;
            request.loop = false;
            request.ambientSeconds = time;
            // Exactly the normal two-clip branch in TimelineClips.jsx: A/A
            // before overlap; A/B with the selected shared overlap curve;
            // B/B after A ends. Source clocks are independent of this curve.
            const bool before = time < startB;
            const bool after = time >= endA;
            request.motionPath = after ? pathB.wstring() : pathA.wstring();
            request.motionPathB = before ? pathA.wstring() : pathB.wstring();
            request.seconds = after ? timeB : timeA;
            request.secondsB = before ? timeA : timeB;
            const double progress = std::clamp((time - startB) / (endA - startB), 0.0, 1.0);
            const double curvedWeight = transitionWeight(progress, curve);
            require(std::isfinite(curvedWeight) && curvedWeight >= 0 &&
                curvedWeight <= (curve == 2 || curve == 4 || curve == 5 ? 1.1 : 1.0),
                "Transition curve exceeded its supported range.");
            request.blend = before || after ? 0.0f : static_cast<float>(curvedWeight);
            auto rendered = l2dae::render(request);
            const auto visible = visiblePixels(rendered);
            require(visible > 0, "Transition frame is fully transparent.");
            const auto file = frameName(frame) + L".png";
            writePng(output / L"transition" / file, rendered);

            auto referenceA = request;
            referenceA.motionPath = pathA.wstring();
            referenceA.motionPathB.clear();
            referenceA.seconds = timeA;
            referenceA.secondsB = 0;
            referenceA.blend = 0;
            const auto imageA = l2dae::render(referenceA);
            writePng(output / L"single-A" / file, imageA);
            auto referenceB = referenceA;
            referenceB.motionPath = pathB.wstring();
            referenceB.seconds = timeB;
            const auto imageB = l2dae::render(referenceB);
            writePng(output / L"single-B" / file, imageB);
            if (before) {
                samePixels(rendered, imageA, "Timeline A/A differs from standalone A before overlap.");
                ++boundaryComparisons;
            } else if (after) {
                samePixels(rendered, imageB, "Timeline B/B differs from standalone B after overlap.");
                ++boundaryComparisons;
            }
            const auto delta = samples.empty() ? Difference{} : difference(samples.back().image, rendered);
            manifest << frame << ',' << time << ',' << (after ? 'B' : 'A') << ',' << (before ? 'A' : 'B')
                << ',' << request.seconds << ',' << request.secondsB << ',' << progress << ',' << curve
                << ',' << request.blend << ',' << visible
                << ',' << delta.pixels << ',' << delta.alphaPixels << ',' << delta.maximum << ',' << delta.absoluteSum << '\n';
            std::cout << "FRAME " << frame << " time_A=" << request.seconds << " time_B=" << request.secondsB
                << " progress=" << progress << " curve=" << curve << " blend=" << request.blend
                << " visible=" << visible << " changed_pixels=" << delta.pixels
                << " changed_alpha=" << delta.alphaPixels << " max_delta=" << delta.maximum
                << " absolute_delta=" << delta.absoluteSum << std::endl;
            samples.push_back({frame, request, std::move(rendered)});
        }
        manifest.close();
        std::size_t failures = 0;
        for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
            const auto actual = l2dae::render(it->request);
            if (actual.rgba != it->image.rgba) {
                ++failures;
                describeDifference(it->request, it->image, actual);
                writePng(output / (frameName(it->frameNumber) + L"-reverse.png"), actual);
            }
        }
        std::cout << "REVISIT exact_comparisons=" << samples.size() << " failed=" << failures << std::endl;
        require(failures == 0, "Reverse-order transition rendering changed pixels.");

        Gate gate;
        std::mutex errorMutex;
        std::exception_ptr error;
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < 4; ++worker) {
            threads.emplace_back([&, worker] {
                gate.wait();
                try {
                    for (std::size_t i = worker; i < samples.size(); i += 4)
                        samePixels(l2dae::render(samples[i].request), samples[i].image,
                            "Concurrent transition rendering changed pixels.");
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!error) error = std::current_exception();
                }
            });
        }
        gate.start(4);
        for (auto& thread : threads) thread.join();
        if (error) std::rethrow_exception(error);
        std::cout << "CONCURRENCY threads=4 exact_comparisons=" << samples.size() << std::endl;
        const auto stats = l2dae::rendererStats();
        l2dae::releaseRenderer();
        std::cout << "PASS: " << samples.size() << " transition frames; " << boundaryComparisons
            << " standalone boundary comparisons; deterministic reverse seeking and 4-thread rendering; frames="
            << stats.framesRendered << "; elapsed_s="
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        l2dae::releaseRenderer();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
