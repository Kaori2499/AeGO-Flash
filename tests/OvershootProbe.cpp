// Independent real-model overshoot oracle: endpoint A/B motions are blended
// above one and compared with a separately authored constant target C motion.
// Original model and motion assets remain read-only.
#include "Renderer.h"
#include <CubismModelSettingJson.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <Id/CubismId.hpp>
#include <Utils/CubismString.hpp>
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
#include <iterator>
#include <limits>
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

struct Sample { l2dae::RenderRequest request; l2dae::RenderResult image; };
struct ParameterRange { float minimum = 0, maximum = 0; };

std::vector<unsigned char> bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "Cannot read test model metadata or MOC.");
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

ParameterRange parameterRange(const std::filesystem::path& path, const std::string& id) {
    // listMotions starts the renderer's Framework runtime. Only this main thread
    // uses SDK objects here; all are destroyed before render worker tests begin.
    l2dae::listMotions(path.wstring());
    namespace Csm = Live2D::Cubism::Framework;
    const auto settingsBytes = bytes(path);
    Csm::CubismModelSettingJson settings(settingsBytes.data(), static_cast<int>(settingsBytes.size()));
    require(settings.GetJsonPointer() != nullptr, "Invalid model metadata.");
    const auto mocBytes = bytes(path.parent_path() / std::filesystem::u8path(settings.GetModelFileName()));
    auto* moc = Csm::CubismMoc::Create(mocBytes.data(), static_cast<int>(mocBytes.size()), true);
    require(moc != nullptr, "Cannot inspect model parameter range.");
    auto* model = moc->CreateModel();
    if (!model) { Csm::CubismMoc::Delete(moc); throw std::runtime_error("Cannot create test metadata model."); }
    ParameterRange result;
    bool found = false;
    for (int i = 0; i < model->GetParameterCount(); ++i) {
        if (id == model->GetParameterId(i)->GetString().GetRawString()) {
            result = {model->GetParameterMinimumValue(i), model->GetParameterMaximumValue(i)};
            found = true;
            break;
        }
    }
    moc->DeleteModel(model);
    Csm::CubismMoc::Delete(moc);
    require(found && std::isfinite(result.minimum) && std::isfinite(result.maximum) && result.minimum < result.maximum,
        "Chosen deformation parameter is absent or has no usable range.");
    return result;
}

std::string sdkExactNumber(float value) {
    // SDK R5's decimal parser accumulates fractional digits in float, so even
    // max_digits10 text need not round-trip like strtof. A reference fixture
    // must reach the mathematically selected float exactly; otherwise comparing
    // raster pixels measures fixture serialization error, not interpolation.
    // Find a decimal spelling with that exact SDK interpretation. This is test
    // fixture generation only; production parsing and the pixel oracle stay strict.
    const auto spelling = [](double candidate) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(18) << candidate;
        return out.str();
    };
    const auto readsExactly = [value](const std::string& text) {
        Live2D::Cubism::Framework::csmInt32 end = 0;
        const float parsed = Live2D::Cubism::Framework::Utils::CubismString::StringToFloat(
            text.c_str(), static_cast<int>(text.size()), 0, &end);
        return end == static_cast<int>(text.size()) && parsed == value;
    };
    auto text = spelling(value);
    if (readsExactly(text)) return text;
    const double step = std::max(
        static_cast<double>(std::nextafter(value, std::numeric_limits<float>::infinity())) - value,
        static_cast<double>(value) - std::nextafter(value, -std::numeric_limits<float>::infinity())) / 64.0;
    for (int offset = 1; offset <= 512; ++offset) {
        for (int direction : {-1, 1}) {
            text = spelling(static_cast<double>(value) + direction * offset * step);
            if (readsExactly(text)) {
                std::cout << "FIXTURE_SDK_FLOAT exact_value=" << std::setprecision(17) << value
                    << " decimal_spelling=" << text << '\n';
                return text;
            }
        }
    }
    throw std::runtime_error("Cannot encode an exact synthetic target with the SDK decimal parser.");
}

void writeMotion(const std::filesystem::path& path, const std::string& parameter, float value, float opacity) {
    require(parameter.find_first_of("\"\\\r\n") == std::string::npos, "Unsupported diagnostic parameter ID.");
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(stream.good(), "Cannot write synthetic overshoot fixture.");
    const auto encodedValue = sdkExactNumber(value), encodedOpacity = sdkExactNumber(opacity);
    stream
        << "{\"Version\":3,\"Meta\":{\"Duration\":1,\"Fps\":30,\"Loop\":false,"
        "\"AreBeziersRestricted\":true,\"FadeInTime\":0,\"FadeOutTime\":0,\"CurveCount\":2,"
        "\"TotalSegmentCount\":2,\"TotalPointCount\":4,\"UserDataCount\":0,\"TotalUserDataSize\":0},"
        "\"Curves\":[{\"Target\":\"Model\",\"Id\":\"Opacity\",\"Segments\":[0," << encodedOpacity << ",0,1," << encodedOpacity
        << "]},{\"Target\":\"Parameter\",\"Id\":\"" << parameter
        << "\",\"FadeInTime\":0,\"FadeOutTime\":0,\"Segments\":[0," << encodedValue << ",0,1," << encodedValue
        << "]}],\"UserData\":[]}\n";
    stream.close();
    require(stream.good(), "Cannot finish synthetic overshoot fixture.");
}

std::size_t differentPixels(const l2dae::RenderResult& a, const l2dae::RenderResult& b) {
    require(a.rgba.size() == b.rgba.size(), "Frame sizes differ.");
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4)
        if (!std::equal(a.rgba.begin() + i, a.rgba.begin() + i + 4, b.rgba.begin() + i)) ++count;
    return count;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: Live2DOvershootProbe model3.json output-folder [parameter=ParamAngleZ]\n";
        return 2;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const auto model = std::filesystem::absolute(argv[1]);
        const auto output = std::filesystem::absolute(argv[2]);
        const std::string parameter = argc > 3 ? std::filesystem::path(argv[3]).u8string() : "ParamAngleZ";
        require(std::filesystem::is_regular_file(model), "Model is missing.");
        std::filesystem::create_directories(output / L"synthetic-fixtures");
        const auto range = parameterRange(model, parameter);
        const float valueA = range.minimum + (range.maximum - range.minimum) * 0.25f;
        const float valueB = range.minimum + (range.maximum - range.minimum) * 0.65f;
        const auto motionA = output / L"synthetic-fixtures" / L"source-A.motion3.json";
        const auto motionB = output / L"synthetic-fixtures" / L"source-B.motion3.json";
        writeMotion(motionA, parameter, valueA, 1);
        writeMotion(motionB, parameter, valueB, 0.65f);
        l2dae::RenderRequest request;
        request.modelPath = model.wstring();
        request.motionPath = motionA.wstring();
        request.motionPathB = motionB.wstring();
        request.seconds = request.secondsB = 0.5;
        request.width = 540;
        request.height = 960;
        request.loop = false;
        request.blend = 1;
        const auto endpointB = l2dae::render(request);
        writePng(output / L"endpoint-B.png", endpointB);
        require(visiblePixels(endpointB) > 0, "Endpoint B is transparent.");
        std::cout << std::setprecision(10) << "MODEL " << model.filename().u8string()
            << " parameter=" << parameter << " range=" << range.minimum << ':' << range.maximum
            << " A=" << valueA << " B=" << valueB << " opacity_A=1 opacity_B=0.65\n";
        const std::array<float, 5> weights{1.01f, 1.025f, 1.0496f, 1.075f, 1.1f};
        std::vector<Sample> samples;
        std::size_t referenceFailures = 0, visiblyOvershot = 0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            request.blend = weights[i];
            // Construct a complete C action directly from the mathematical
            // target value; it does not use the runtime's blend path at all.
            const float expectedValue = std::clamp(static_cast<float>(static_cast<double>(valueA) +
                (static_cast<double>(valueB) - valueA) * weights[i]), range.minimum, range.maximum);
            const auto targetPath = output / L"synthetic-fixtures" / (L"reference-C-" + std::to_wstring(i) + L".motion3.json");
            writeMotion(targetPath, parameter, expectedValue, 0.65f);
            const auto actual = l2dae::render(request);
            auto reference = request;
            reference.motionPath = targetPath.wstring();
            reference.motionPathB.clear();
            reference.blend = 0;
            const auto expected = l2dae::render(reference);
            const auto fromB = differentPixels(actual, endpointB);
            const auto fromReference = differentPixels(actual, expected);
            if (fromB > 0) ++visiblyOvershot;
            if (fromReference > 0) { ++referenceFailures; describeDifference(request, expected, actual); }
            std::cout << "OVERSHOOT weight=" << weights[i] << " expected_parameter=" << expectedValue
                << " pixels_different_from_B=" << fromB << " pixels_different_from_reference_C=" << fromReference << std::endl;
            writePng(output / (L"overshoot-" + std::to_wstring(i) + L"-actual.png"), actual);
            writePng(output / (L"overshoot-" + std::to_wstring(i) + L"-reference-C.png"), expected);
            samples.push_back({request, actual});
        }
        // Returning to exactly one must restore B without residual physics,
        // source order, model opacity or cache state from the previous overshoot.
        request.blend = 1;
        samePixels(l2dae::render(request), endpointB, "Settling from overshoot did not return exactly to B.");
        samples.push_back({request, endpointB});

        const auto clampA = output / L"synthetic-fixtures" / L"range-min.motion3.json";
        const auto clampB = output / L"synthetic-fixtures" / L"range-max.motion3.json";
        writeMotion(clampA, parameter, range.minimum, 1);
        writeMotion(clampB, parameter, range.maximum, 0.65f);
        auto bounded = request;
        bounded.motionPath = clampA.wstring();
        bounded.motionPathB = clampB.wstring();
        const auto atMaximum = l2dae::render(bounded);
        bounded.blend = 1.1f;
        const auto clamped = l2dae::render(bounded);
        samePixels(clamped, atMaximum, "Overshoot went beyond the model parameter limit or wrapped it.");
        samples.push_back({bounded, clamped});
        std::cout << "MODEL_RANGE_CLAMP exact=1 MODEL_OPACITY held_at_B=0.65 SETTLE exact=1\n";

        std::size_t rejected = 0;
        for (float invalid : {-0.01f, 1.1001f, std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN()}) {
            auto bad = request;
            bad.blend = invalid;
            try { l2dae::render(bad); }
            catch (const std::exception&) { ++rejected; continue; }
            throw std::runtime_error("Renderer accepted an out-of-range or non-finite blend.");
        }
        for (auto it = samples.rbegin(); it != samples.rend(); ++it)
            samePixels(l2dae::render(it->request), it->image, "Reverse-order overshoot changed pixels.");
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
                            "Concurrent overshoot differs from its serial reference.");
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!error) error = std::current_exception();
                }
            });
        }
        gate.start(4);
        for (auto& thread : threads) thread.join();
        if (error) std::rethrow_exception(error);
        require(referenceFailures == 0, "Overshoot does not match the independently authored C action exactly.");
        require(visiblyOvershot == weights.size(), "Overshoot never moved visibly beyond B for one or more weights.");
        std::cout << "PASS: " << weights.size() << " independent real-geometry pixel oracles; " << visiblyOvershot
            << " visibly different from B; model range and opacity bounded; reverse=" << samples.size()
            << " concurrent=" << samples.size() << " rejected=" << rejected << "; elapsed_s="
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << '\n';
        l2dae::releaseRenderer();
        return 0;
    } catch (const std::exception& error) {
        l2dae::releaseRenderer();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
