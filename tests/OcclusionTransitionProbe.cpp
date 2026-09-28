// Real Hotaru model regression for a parameter that changes only draw order.
// Synthetic motions are written to the supplied output folder; original assets
// are read-only. Full endpoint renders are the independent pixel-blend oracle.
#include "Renderer.h"
#include "MotionJson.h"
#include <Motion/CubismMotionJson.hpp>
#include <Id/CubismId.hpp>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iterator>
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
    l2dae::RenderResult image;
};
struct Difference {
    std::size_t pixels = 0;
    std::size_t channelsOverTolerance = 0;
    unsigned maximum = 0;
    int minimumSigned = 0, maximumSigned = 0;
};
Difference compare(const l2dae::RenderResult& actual, const l2dae::RenderResult& expected,
    unsigned tolerance) {
    require(actual.width == expected.width && actual.height == expected.height &&
        actual.rgba.size() == expected.rgba.size(), "Frame dimensions differ.");
    Difference result;
    for (std::size_t pixel = 0; pixel < actual.rgba.size(); pixel += 4) {
        bool changed = false;
        for (std::size_t c = 0; c < 4; ++c) {
            const int delta = static_cast<int>(actual.rgba[pixel + c]) -
                static_cast<int>(expected.rgba[pixel + c]);
            const unsigned magnitude = static_cast<unsigned>(std::abs(delta));
            changed = changed || delta != 0;
            result.maximum = std::max(result.maximum, magnitude);
            result.minimumSigned = std::min(result.minimumSigned, delta);
            result.maximumSigned = std::max(result.maximumSigned, delta);
            if (magnitude > tolerance) ++result.channelsOverTolerance;
        }
        if (changed) ++result.pixels;
    }
    return result;
}
void logDifference(const char* label, float weight, const Difference& delta) {
    std::cout << label << " weight=" << weight << " changed_pixels=" << delta.pixels
        << " max_channel_delta=" << delta.maximum << " signed_range=" << delta.minimumSigned
        << ':' << delta.maximumSigned << " channels_over_tolerance=" << delta.channelsOverTolerance << std::endl;
}

l2dae::RenderResult mixEndpoints(const l2dae::RenderResult& a, const l2dae::RenderResult& b,
    float weight) {
    require(a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size(),
        "Endpoint dimensions differ.");
    auto result = a;
    const double mix = weight;
    // Renderer API supplies premultiplied RGBA bytes. Compare the blend in the
    // same representation without introducing alpha unpremultiplication error.
    for (std::size_t i = 0; i < result.rgba.size(); ++i)
        result.rgba[i] = static_cast<std::uint8_t>(std::lround(
            static_cast<double>(a.rgba[i]) * (1.0 - mix) + static_cast<double>(b.rgba[i]) * mix));
    return result;
}

struct ConstantParameter { std::string id; float value; };
std::vector<ConstantParameter> basePose(const std::filesystem::path& path) {
    // This public call starts the renderer's Framework runtime. Parse only on
    // this main thread, before any render workers are started; retain no SDK
    // objects across releaseRenderer().
    require(l2dae::motionDuration(path.wstring()) > 0, "Invalid base motion.");
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Cannot read base motion.");
    const std::vector<unsigned char> raw((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const auto bytes = l2dae::normalizeMotionJson(raw).bytes;
    namespace Csm = Live2D::Cubism::Framework;
    Csm::CubismMotionJson motion(bytes.data(), static_cast<int>(bytes.size()));
    require(motion.HasConsistency(), "Base motion has inconsistent curve metadata.");
    std::vector<ConstantParameter> result;
    for (int i = 0; i < motion.GetMotionCurveCount(); ++i) {
        if (std::strcmp(motion.GetMotionCurveTarget(i), "Parameter") != 0) continue;
        require(motion.GetMotionCurveSegmentCount(i) >= 2, "Base parameter has no first point.");
        result.push_back({motion.GetMotionCurveId(i)->GetString().GetRawString(),
            motion.GetMotionCurveSegment(i, 1)});
    }
    require(!result.empty(), "Base motion has no parameter curves.");
    return result;
}
std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(c);
        else out << c;
    }
    out << '"';
    return out.str();
}
void writeMotion(const std::filesystem::path& path, std::vector<ConstantParameter> pose,
    const std::string& parameter, float value) {
    bool found = false;
    for (auto& item : pose) {
        if (item.id == parameter) { item.value = value; found = true; }
    }
    require(found, "Base motion does not contain the chosen draw-order parameter.");
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(stream.good(), "Cannot write synthetic motion fixture.");
    stream << std::setprecision(9);
    stream << "{\"Version\":3,\"Meta\":{\"Duration\":1,\"Fps\":30,\"Loop\":false,"
        "\"AreBeziersRestricted\":true,\"FadeInTime\":0,\"FadeOutTime\":0,\"CurveCount\":" << pose.size()
        << ",\"TotalSegmentCount\":" << pose.size() << ",\"TotalPointCount\":" << pose.size() * 2
        << ",\"UserDataCount\":0,\"TotalUserDataSize\":0},\"Curves\":[";
    for (std::size_t i = 0; i < pose.size(); ++i) {
        if (i) stream << ',';
        stream << "{\"Target\":\"Parameter\",\"Id\":" << jsonString(pose[i].id)
            << ",\"FadeInTime\":0,\"FadeOutTime\":0,\"Segments\":[0," << pose[i].value
            << ",0,1," << pose[i].value << "]}";
    }
    stream << "],\"UserData\":[]}\n";
    stream.close();
    require(stream.good(), "Cannot finish synthetic motion fixture.");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 7) {
        std::cerr << "Usage: Live2DOcclusionTransitionProbe Hotaru.model3.json base-motion3.json output-folder "
            "[parameter-id value-A value-B]\n";
        return 2;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const auto model = std::filesystem::absolute(argv[1]);
        const auto baseMotion = std::filesystem::absolute(argv[2]);
        const auto output = std::filesystem::absolute(argv[3]);
        require(std::filesystem::is_regular_file(model), "Model is missing.");
        require(std::filesystem::is_regular_file(baseMotion), "Base motion is missing.");
        const std::string parameter = argc == 7 ? std::filesystem::path(argv[4]).u8string() : "paramArmL_layer2";
        const float valueA = argc == 7 ? std::stof(argv[5]) : 0.0f;
        const float valueB = argc == 7 ? std::stof(argv[6]) : 1.0f;
        require(std::isfinite(valueA) && std::isfinite(valueB) && valueA != valueB,
            "Diagnostic parameter values must be finite and different.");
        const auto fixedPose = basePose(baseMotion);
        const auto fixtures = output / L"synthetic-occlusion-fixture";
        std::filesystem::create_directories(fixtures);
        const auto motionA = fixtures / L"order-A.motion3.json";
        const auto motionB = fixtures / L"order-B.motion3.json";
        writeMotion(motionA, fixedPose, parameter, valueA);
        writeMotion(motionB, fixedPose, parameter, valueB);
        require(l2dae::motionDuration(motionA.wstring()) == 1 &&
            l2dae::motionDuration(motionB.wstring()) == 1, "Synthetic fixture duration is invalid.");

        l2dae::RenderRequest request;
        request.modelPath = model.wstring();
        request.motionPath = motionA.wstring();
        request.seconds = 0.5;
        request.loop = false;
        request.width = 540;
        request.height = 960;
        const auto endpointA = l2dae::render(request);
        request.motionPath = motionB.wstring();
        const auto endpointB = l2dae::render(request);
        require(visiblePixels(endpointA) > 0 && visiblePixels(endpointB) > 0,
            "Synthetic endpoint render is transparent.");
        writePng(output / L"endpoint-A.png", endpointA);
        writePng(output / L"endpoint-B.png", endpointB);
        const auto endpointDifference = compare(endpointA, endpointB, 0);
        logDifference("ENDPOINT", 0, endpointDifference);
        require(endpointDifference.pixels > 0, "Fixture endpoints do not change visible occlusion.");

        request.motionPath = motionA.wstring();
        request.motionPathB = motionB.wstring();
        request.secondsB = request.seconds;
        std::vector<Sample> samples;
        std::size_t oracleFailures = 0, symmetryFailures = 0;
        std::cout << std::setprecision(9) << "MODEL " << model.filename().u8string()
            << " base_pose=" << baseMotion.filename().u8string() << " constant_parameters=" << fixedPose.size()
            << " parameter=" << parameter << " A=" << valueA << " B=" << valueB
            << " fixed_time=0.5 width=540 height=960\n";
        const std::array<float, 9> weights{0.1f, 0.25f, 0.49f, 0.49999f, 0.5f, 0.50001f, 0.51f, 0.75f, 0.9f};
        for (std::size_t i = 0; i < weights.size(); ++i) {
            request.blend = weights[i];
            const auto actual = l2dae::render(request);
            const auto expected = mixEndpoints(endpointA, endpointB, weights[i]);
            const auto oracleDelta = compare(actual, expected, 1);
            logDifference("ORACLE", weights[i], oracleDelta);
            if (oracleDelta.channelsOverTolerance != 0) ++oracleFailures;
            const auto stem = std::to_wstring(i) + L"-weight-" + std::to_wstring(weights[i]);
            writePng(output / (stem + L"-actual.png"), actual);
            writePng(output / (stem + L"-expected.png"), expected);
            auto swapped = request;
            std::swap(swapped.motionPath, swapped.motionPathB);
            std::swap(swapped.seconds, swapped.secondsB);
            swapped.blend = 1.0f - request.blend;
            const auto swappedImage = l2dae::render(swapped);
            const auto symmetryDelta = compare(actual, swappedImage, 1);
            logDifference("SYMMETRY", weights[i], symmetryDelta);
            if (symmetryDelta.channelsOverTolerance != 0) {
                ++symmetryFailures;
                writePng(output / (stem + L"-swapped.png"), swappedImage);
            }
            samples.push_back({request, actual});
        }
        // Report the central near-neighbor change independently of the oracle.
        // The bounded oracle errors above remain the actual regression assertion.
        const auto midpoint = compare(samples[3].image, samples[5].image, 0);
        logDifference("MIDPOINT_0.49999_TO_0.50001", 0.5f, midpoint);

        std::size_t revisitFailures = 0;
        for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
            const auto revisited = l2dae::render(it->request);
            if (revisited.rgba != it->image.rgba) {
                ++revisitFailures;
                describeDifference(it->request, it->image, revisited);
            }
        }
        std::cout << "REVISIT exact_comparisons=" << samples.size() << " failed=" << revisitFailures << std::endl;
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
                            "Concurrent occlusion rendering changed pixels.");
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
        std::cout << "SUMMARY oracle_comparisons=" << samples.size() << " oracle_tolerance_bytes=1 oracle_failed="
            << oracleFailures << " symmetry_failed=" << symmetryFailures << std::endl;
        require(oracleFailures == 0, "Occlusion transition differs from independent endpoint blend by more than one byte.");
        require(symmetryFailures == 0, "Occlusion transition is not symmetric within one byte.");
        require(revisitFailures == 0, "Reverse-order occlusion rendering changed pixels.");
        l2dae::releaseRenderer();
        std::cout << "PASS: pure draw-order transition matches independent endpoint blend; reverse and 4-thread exact; elapsed_s="
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        l2dae::releaseRenderer();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
