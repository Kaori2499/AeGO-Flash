// Optional acceptance probe for a real model supplied by the developer. Assets
// and reference motions remain outside the source/release archive. References
// are independently prepared, valid motion3.json files, not repaired here.
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
    l2dae::RenderRequest request;
    l2dae::RenderResult frame;
};

std::wstring referencePath(const std::filesystem::path& folder, const std::wstring& source) {
    const auto file = folder / std::filesystem::path(source).filename();
    require(std::filesystem::is_regular_file(file), "Missing independently prepared reference motion.");
    return file.wstring();
}

void testConcurrent(const std::vector<Sample>& samples,
    const std::vector<l2dae::MotionEntry>& motions, const std::filesystem::path& referenceFolder,
    std::size_t& referenceComparisons) {
    std::vector<Sample> mixed;
    for (std::size_t i = 0; i < 8; ++i) {
        auto request = samples[(i * 7 + 1) % samples.size()].request;
        request.motionPathB = motions[(i + 1) % motions.size()].path;
        request.secondsB = l2dae::motionDuration(request.motionPathB) * (0.19 + static_cast<double>(i % 3) * 0.23);
        request.blend = static_cast<float>(i % 5) * 0.25f;
        mixed.push_back({request, l2dae::render(request)});
        if (!referenceFolder.empty()) {
            auto reference = request;
            reference.motionPath = referencePath(referenceFolder, request.motionPath);
            reference.motionPathB = referencePath(referenceFolder, request.motionPathB);
            samePixels(l2dae::render(reference), mixed.back().frame,
                "Mixed original motions differ from the independent stepped reference.");
            ++referenceComparisons;
        }
    }
    Gate gate;
    std::mutex errorMutex;
    std::exception_ptr error;
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < 4; ++index) {
        threads.emplace_back([&, index] {
            gate.wait();
            try {
                for (std::size_t i = index; i < mixed.size(); i += 4)
                    samePixels(l2dae::render(mixed[i].request), mixed[i].frame,
                        "Concurrent mixed-motion render differs from serial reference.");
            } catch (...) {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (!error) error = std::current_exception();
            }
        });
    }
    gate.start(4);
    for (auto& thread : threads) thread.join();
    if (error) std::rethrow_exception(error);
    for (auto sample = mixed.rbegin(); sample != mixed.rend(); ++sample)
        samePixels(l2dae::render(sample->request), sample->frame,
            "Serial mixed-motion output changed after concurrent rendering.");
    std::cout << "CONCURRENCY threads=4 mixed_requests=" << mixed.size()
        << " exact_comparisons=" << mixed.size() * 2 << '\n';
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: Live2DMotionCompatibilityProbe model3.json [output-folder [reference-motion-folder]]\n";
        return 2;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const auto model = std::filesystem::absolute(argv[1]);
        require(std::filesystem::is_regular_file(model), "Model file does not exist.");
        const auto output = argc >= 3 ? std::filesystem::absolute(argv[2]) : std::filesystem::path{};
        const auto referenceFolder = argc >= 4 ? std::filesystem::absolute(argv[3]) : std::filesystem::path{};
        if (!output.empty()) std::filesystem::create_directories(output);
        if (!referenceFolder.empty())
            require(std::filesystem::is_directory(referenceFolder), "Reference folder does not exist.");
        const auto motions = l2dae::listMotions(model.wstring());
        require(!motions.empty(), "The real model contains no motions to test.");
        std::cout << "MODEL " << model.filename().u8string() << " motions=" << motions.size()
            << " width=384 height=512\n";

        std::vector<Sample> samples;
        std::size_t visibleFrames = 0;
        std::size_t variedMotions = 0;
        std::size_t referenceComparisons = 0;
        for (std::size_t motionIndex = 0; motionIndex < motions.size(); ++motionIndex) {
            const auto& motion = motions[motionIndex];
            const auto filename = std::filesystem::path(motion.path).filename();
            const double duration = l2dae::motionDuration(motion.path);
            require(std::isfinite(duration) && duration > 0, "A real motion has an invalid duration.");
            const std::wstring reference = referenceFolder.empty() ? std::wstring{} :
                referencePath(referenceFolder, motion.path);
            if (!reference.empty())
                require(std::abs(l2dae::motionDuration(reference) - duration) < 1e-6,
                    "Original and reference motion durations differ.");
            const std::array<double, 4> times{0.0, duration * 0.37, duration * 0.73, duration};
            const std::size_t firstSample = samples.size();
            std::size_t visibleForMotion = 0;
            std::size_t changedFromFirst = 0;
            const bool saveImages = filename == L"mtn_action_01.motion3.json" ||
                (motionIndex == 0 && std::none_of(motions.begin(), motions.end(), [](const auto& entry) {
                    return std::filesystem::path(entry.path).filename() == L"mtn_action_01.motion3.json";
                }));
            for (std::size_t i = 0; i < times.size(); ++i) {
                l2dae::RenderRequest request;
                request.modelPath = model.wstring();
                request.motionPath = motion.path;
                request.seconds = times[i];
                request.loop = false;
                request.width = 384;
                request.height = 512;
                auto frame = l2dae::render(request);
                const auto visible = visiblePixels(frame);
                if (visible > 0) { ++visibleForMotion; ++visibleFrames; }
                if (i > 0 && frame.rgba != samples[firstSample].frame.rgba) ++changedFromFirst;
                std::cout << "FRAME " << filename.u8string() << " t=" << std::setprecision(10)
                    << times[i] << " visible_pixels=" << visible << '\n';
                l2dae::RenderResult referenceFrame;
                if (!reference.empty()) {
                    auto referenceRequest = request;
                    referenceRequest.motionPath = reference;
                    referenceFrame = l2dae::render(referenceRequest);
                    samePixels(frame, referenceFrame,
                        "Original motion differs from its independently prepared stepped reference.");
                    ++referenceComparisons;
                }
                if (saveImages && !output.empty() && i < 3) {
                    const auto stem = std::filesystem::path(filename).stem().wstring();
                    writePng(output / (stem + L"-original-" + std::to_wstring(i) + L".png"), frame);
                    if (!reference.empty())
                        writePng(output / (stem + L"-reference-" + std::to_wstring(i) + L".png"), referenceFrame);
                }
                samples.push_back({request, std::move(frame)});
            }
            if (changedFromFirst > 0) ++variedMotions;
            std::cout << "MOTION " << filename.u8string() << " duration=" << duration
                << " visible_frames=" << visibleForMotion << '/' << times.size()
                << " different_from_initial=" << changedFromFirst << '\n';
        }
        // Some valid motions intentionally hide a model or hold its pose. Require
        // visible animation somewhere in the complete real-model workload only.
        require(visibleFrames > 0, "Every real-model frame is fully transparent.");
        require(variedMotions > 0, "No real motion changes the rendered model across sampled times.");
        std::size_t revisitFailures = 0;
        for (std::size_t reverseIndex = samples.size(); reverseIndex > 0; --reverseIndex) {
            const auto& sample = samples[reverseIndex - 1];
            const auto revisited = l2dae::render(sample.request);
            if (revisited.width != sample.frame.width || revisited.height != sample.frame.height ||
                revisited.rgba != sample.frame.rgba) {
                ++revisitFailures;
                describeDifference(sample.request, sample.frame, revisited);
                if (!output.empty()) {
                    const auto stem = std::filesystem::path(sample.request.motionPath).stem().wstring()
                        + L"-revisit-" + std::to_wstring(reverseIndex - 1);
                    writePng(output / (stem + L"-expected.png"), sample.frame);
                    writePng(output / (stem + L"-actual.png"), revisited);
                    std::cout << "REVISIT_IMAGES sample=" << reverseIndex - 1
                        << " expected=" << (output / (stem + L"-expected.png")).u8string()
                        << " actual=" << (output / (stem + L"-actual.png")).u8string() << '\n';
                }
            }
        }
        std::cout << "REVISIT exact_comparisons=" << samples.size()
            << " passed=" << samples.size() - revisitFailures << " failed=" << revisitFailures << std::endl;
        require(revisitFailures == 0, "A backwards/random-access real-motion revisit changed pixels.");
        testConcurrent(samples, motions, referenceFolder, referenceComparisons);
        const auto stats = l2dae::rendererStats();
        std::cout << "STATS workers=" << stats.workerCount << " peak_cpu=" << stats.peakCpuConcurrency
            << " frames=" << stats.framesRendered << '\n';
        l2dae::releaseRenderer();
        samePixels(l2dae::render(samples.front().request), samples.front().frame,
            "Fresh renderer changed the first real-motion frame.");
        l2dae::releaseRenderer();
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "REFERENCE exact_comparisons=" << referenceComparisons << '\n';
        if (!referenceFolder.empty())
            std::cout << "SCOPE: Equality verifies the selected stepped interpretation of non-finite tangents; "
                "it does not recover or establish the original author's intended damaged-curve shape.\n";
        std::cout << "PASS: " << motions.size() << " real motions; " << samples.size()
            << " sampled frames; " << visibleFrames << " visible; " << variedMotions
            << " changing motions; deterministic seeking, pose blending, 4-thread rendering"
            << "; elapsed_s=" << elapsed << '\n';
        return 0;
    } catch (const std::exception& error) {
        l2dae::releaseRenderer();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
