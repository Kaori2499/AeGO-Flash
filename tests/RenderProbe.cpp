#include "Renderer.h"
#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void check(HRESULT result, const char* message) { require(SUCCEEDED(result), message); }

struct Bounds { int left, top, right, bottom; std::size_t pixels; };
Bounds alphaBounds(const l2dae::RenderResult& image) {
    Bounds bounds{image.width, image.height, -1, -1, 0};
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4 + 3] > 16) {
                bounds.left = std::min(bounds.left, x);
                bounds.top = std::min(bounds.top, y);
                bounds.right = std::max(bounds.right, x);
                bounds.bottom = std::max(bounds.bottom, y);
                ++bounds.pixels;
            }
        }
    }
    return bounds;
}

std::size_t differingPixels(const l2dae::RenderResult& a, const l2dae::RenderResult& b) {
    require(a.width == b.width && a.height == b.height, "Images have different dimensions.");
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4)
        if (!std::equal(a.rgba.begin() + i, a.rgba.begin() + i + 4, b.rgba.begin() + i)) ++count;
    return count;
}

void writePng(const std::wstring& path, const l2dae::RenderResult& image) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) check(initialized, "COM initialization failed.");
    struct Uninitialize { bool active; ~Uninitialize() { if (active) CoUninitialize(); } } cleanup{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf())), "Cannot create WIC encoder.");
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(stream.GetAddressOf()), "Cannot create output stream.");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Cannot open PNG output.");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()), "Cannot create PNG encoder.");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Cannot initialize PNG encoder.");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    check(encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf()), "Cannot create PNG frame.");
    check(frame->Initialize(options.Get()), "Cannot initialize PNG frame.");
    check(frame->SetSize(image.width, image.height), "Cannot set PNG dimensions.");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "Cannot set PNG pixel format.");
    require(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "PNG encoder did not accept BGRA.");
    auto straight = image.rgba;
    // PNG stores straight alpha; the AE renderer returns premultiplied pixels.
    for (std::size_t i = 0; i < straight.size(); i += 4) {
        const unsigned alpha = straight[i + 3];
        for (std::size_t c = 0; c < 3; ++c)
            straight[i + c] = alpha ? static_cast<std::uint8_t>(std::min(255u,
                (static_cast<unsigned>(straight[i + c]) * 255u + alpha / 2) / alpha)) : 0;
        std::swap(straight[i], straight[i + 2]);
    }
    check(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(straight.size()), straight.data()), "Cannot write PNG pixels.");
    check(frame->Commit(), "Cannot finish PNG frame.");
    check(encoder->Commit(), "Cannot finish PNG file.");
}

class ExpressionFixtures {
    std::filesystem::path parent_ = std::filesystem::temp_directory_path();
public:
    std::filesystem::path root = parent_ / (L"Live2DAE-expression-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    ExpressionFixtures() { require(std::filesystem::create_directory(root), "Cannot create private expression test fixtures."); }
    ~ExpressionFixtures() {
        if (root.parent_path() == parent_ && root.filename().wstring().rfind(L"Live2DAE-expression-probe-", 0) == 0) {
            std::error_code error; std::filesystem::remove_all(root, error);
        }
    }
    std::wstring write(const std::wstring& name, const std::string& text) const {
        const auto path = root / name;
        std::ofstream output(path, std::ios::binary);
        output << text;
        require(static_cast<bool>(output), "Cannot write expression test fixture.");
        return path.wstring();
    }
    std::wstring expression(const std::wstring& name, double value, const std::string& blend, const std::string& parameter = "ParamAngleX") const {
        std::ostringstream json; json.imbue(std::locale::classic()); json << std::setprecision(17);
        json << "{\"Type\":\"Live2D Expression\",\"FadeInTime\":99,\"FadeOutTime\":99,\"Parameters\":[{\"Id\":" << std::quoted(parameter) << ",\"Value\":"
            << value << ",\"Blend\":\"" << blend << "\"}]}";
        return write(name, json.str());
    }
    std::wstring motion(double value, const std::string& parameter = "ParamAngleX") const {
        std::ostringstream json; json.imbue(std::locale::classic()); json << std::setprecision(17);
        json << "{\n\"Version\":3,\n\"Meta\":{\n\"Duration\":1,\n\"Fps\":30,\n\"Loop\":true,\n"
            "\"AreBeziersRestricted\":true,\n\"CurveCount\":1,\n\"TotalSegmentCount\":1,\n\"TotalPointCount\":2,\n"
            "\"UserDataCount\":0,\n\"TotalUserDataSize\":0\n},\n\"Curves\":[{\n\"Target\":\"Parameter\",\n"
            "\"Id\":" << std::quoted(parameter) << ",\n\"Segments\":[\n0," << value << ",0,1," << value << "\n]\n}]\n}\n";
        return write(L"constant-" + std::wstring(parameter.begin(), parameter.end()) + L"-" + std::to_wstring(value) + L".motion3.json", json.str());
    }
};

// Borrow the renderer's already-initialized Framework, without changing its
// lifetime. The reference owns only this separate Moc/Model pair and asks the
// official weighted setter to handle the real model's range/repeat behavior.
template<class Callback>
auto withReferenceModel(const std::wstring& modelPath, Callback callback) {
    using namespace Live2D::Cubism::Framework;
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        require(static_cast<bool>(input), "Cannot open SDK overwrite reference file.");
        std::vector<csmByte> bytes((std::istreambuf_iterator<char>(input)), {});
        require(!bytes.empty(), "Empty SDK overwrite reference file.");
        return bytes;
    };
    const auto settingBytes = read(modelPath);
    CubismModelSettingJson settings(settingBytes.data(), static_cast<csmSizeInt>(settingBytes.size()));
    const auto mocBytes = read(std::filesystem::path(modelPath).parent_path() / std::filesystem::u8path(settings.GetModelFileName()));
    struct OwnedModel {
        CubismMoc* moc = nullptr;
        CubismModel* model = nullptr;
        ~OwnedModel() {
            if (moc && model) moc->DeleteModel(model);
            if (moc) CubismMoc::Delete(moc);
        }
    } owned;
    owned.moc = CubismMoc::Create(mocBytes.data(), static_cast<csmSizeInt>(mocBytes.size()), true);
    require(owned.moc != nullptr, "Cannot create SDK overwrite reference Moc.");
    owned.model = owned.moc->CreateModel();
    require(owned.model != nullptr, "Cannot create SDK overwrite reference model.");
    return callback(*owned.model);
}

std::pair<float, float> overwriteReference(const std::wstring& modelPath, const char* parameter) {
    using namespace Live2D::Cubism::Framework;
    return withReferenceModel(modelPath, [&](CubismModel& model) {
        const auto index = model.GetParameterIndex(CubismFramework::GetIdManager()->GetId(parameter));
        require(index >= 0 && index < model.GetParameterCount(), "Overwrite reference parameter is missing.");
        const float maximum = model.GetParameterMaximumValue(index);
        const float target = maximum + std::max(1.0f, std::abs(maximum));
        model.SetParameterValue(index, 0);
        model.SetParameterValue(index, target, 0.5f);
        return std::make_pair(target, model.GetParameterValue(index));
    });
}

void testExpressions(const l2dae::RenderRequest& original, const std::wstring& previewPath) {
    ExpressionFixtures files;
    const auto baseline = l2dae::render(original);
    auto inactive = original;
    inactive.expressionPathA = (files.root / L"not-present.exp3.json").wstring();
    inactive.expressionPathB = inactive.expressionPathA;
    require(baseline.rgba == l2dae::render(inactive).rgba, "Zero-weight expressions loaded a missing file or changed legacy pixels.");
    for (int invalid = 0; invalid < 4; ++invalid) {
        auto request = original;
        if (invalid == 0) request.expressionWeightA = std::numeric_limits<float>::quiet_NaN();
        if (invalid == 1) request.expressionWeightB = std::numeric_limits<float>::infinity();
        if (invalid == 2) request.expressionWeightA = -0.25f;
        if (invalid == 3) request.expressionWeightB = 1.25f;
        bool rejected = false;
        try { l2dae::render(request); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Invalid expression weight was accepted.");
    }
    const std::vector<std::string> malformed{
        "{", "{\"Parameters\":[]} trailing", "{\"Parameters\":[,]}", "{\"Parameters\":{}}",
        "{\"Parameters\":[{\"Id\":\"x\",\"Value\":0,\"Blend\":\"Unsupported\"}]}",
        "{\"Parameters\":[{\"Id\":\"x\",\"Value\":0,\"Blend\":3}]}",
        "{\"Parameters\":[{\"Id\":\"x\",\"Value\":0,\"Blend\":0.5}]}",
        "{\"Parameters\":[{\"Id\":\"x\",\"Value\":true}]}",
        "{\"Parameters\":[{\"Id\":\"x\",\"Value\":\"0\"}]}",
        "{\"Parameters\":[{\"Id\":\"x\",\"Value\":1e999}]}",
        "{\"Parameters\":[{\"Id\":\"\",\"Value\":0}]}",
        "{\"Parameters\":[{\"Id\":\"x\",\"Value\":0},{\"Id\":\"x\",\"Value\":1}]}",
        "{\"Type\":\"Other\",\"Parameters\":[]}", "{\"Parameters\":[],\"Parameters\":[]}"
    };
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        const auto path = files.write(L"invalid-" + std::to_wstring(i) + L".exp3.json", malformed[i]);
        bool rejected = false;
        try { l2dae::validateExpression(path); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Malformed or unsupported expression was accepted.");
    }
    const auto add12 = files.expression(L"加算12.exp3.json", 12, "Add");
    const auto add20 = files.expression(L"add20.exp3.json", 20, "Add");
    const auto mult2 = files.expression(L"mult2.exp3.json", 2, "Multiply");
    const auto mult3 = files.expression(L"mult3.exp3.json", 3, "Multiply");
    const auto overwrite24 = files.expression(L"overwrite24.exp3.json", 24, "Overwrite");
    const auto overwrite16 = files.expression(L"overwrite16.exp3.json", 16, "Overwrite");
    l2dae::validateExpression(add12);
    const auto exponent = files.write(L"exponent.exp3.json", "{\"Parameters\":[{\"Id\":\"ParamAngleX\",\"Value\":1.2e1}]}");
    l2dae::validateExpression(exponent);
    l2dae::validateExpression(files.write(L"neutral.exp3.json", "{\"Type\":\"Live2D Expression\",\"Parameters\":[]}"));
    l2dae::validateExpression(files.write(L"short-type.exp3.json", "{\"Type\":\"Expression\",\"Parameters\":[]}"));

    auto base = original;
    base.motionPath = files.motion(8);
    base.motionPathB.clear(); base.blend = 0; base.seconds = 0.375; base.secondsB = 0;
    base.expressionPathA.clear(); base.expressionPathB.clear(); base.expressionWeightA = base.expressionWeightB = 0;
    const auto checkCase = [&](const std::wstring& a, float wa, const std::wstring& b, float wb, double expected, const char* failure) {
        auto request = base;
        request.expressionPathA = a; request.expressionWeightA = wa;
        request.expressionPathB = b; request.expressionWeightB = wb;
        const auto actual = l2dae::render(request);
        auto reference = base; reference.motionPath = files.motion(expected);
        require(actual.rgba == l2dae::render(reference).rgba, failure);
        std::swap(request.expressionPathA, request.expressionPathB);
        std::swap(request.expressionWeightA, request.expressionWeightB);
        require(actual.rgba == l2dae::render(request).rgba, "Expression A/B order changed the result.");
        return actual;
    };
    const auto operatorBase = checkCase(add12, 0, L"", 0, 8, "Expression weight 0 changed the pose.");
    const auto operatorApplied = checkCase(add12, 1, L"", 0, 20, "Full additive expression was not applied.");
    const auto syntheticChanged = differingPixels(operatorBase, operatorApplied);
    require(syntheticChanged > 0, "Synthetic expression fixture requires a visible ParamAngleX parameter.");
    checkCase(add12, 0.5f, L"", 0, 14, "Weighted Add semantics are incorrect.");
    checkCase(mult2, 0.5f, L"", 0, 12, "Weighted Multiply semantics are incorrect.");
    checkCase(overwrite24, 0.5f, L"", 0, 16, "Weighted Overwrite semantics are incorrect.");
    checkCase(add12, 0.25f, add20, 0.5f, 21, "Simultaneous additive offsets did not sum.");
    checkCase(mult2, 0.25f, mult3, 0.25f, 14, "Multiplicative deviations did not combine.");
    checkCase(overwrite24, 0.25f, overwrite16, 0.5f, 16, "Overwrite mixing lost the unassigned base weight.");
    checkCase(overwrite24, 0.75f, overwrite16, 0.75f, 20, "Overwrite targets were not normalized above total weight 1.");
    checkCase(add12, 0.5f, mult2, 0.5f, 21, "Add/Multiply component aggregation is incorrect.");
    checkCase(overwrite24, 0.5f, add12, 0.5f, 22, "Overwrite/Add component aggregation is incorrect.");
    checkCase(add12, 0.25f, add12, 0.5f, 17, "Identical expression slots did not combine their weights.");
    checkCase(add12, 0.75f, add12, 0.75f, 20, "Identical expression was applied more than once at full strength.");
    checkCase(exponent, 0.5f, L"", 0, 14, "Compact expression, default Add, or exponent notation parsed incorrectly.");
    const auto unityAdd = files.write(L"unity-add.exp3.json", "{\"Parameters\":[{\"Id\":\"ParamAngleX\",\"Value\":12,\"Blend\":1}]}");
    const auto unityMultiply = files.write(L"unity-multiply.exp3.json", "{\"Parameters\":[{\"Id\":\"ParamAngleX\",\"Value\":2,\"Blend\":2}]}");
    const auto unityOverwrite = files.write(L"unity-overwrite.exp3.json", "{\"Parameters\":[{\"Id\":\"ParamAngleX\",\"Value\":24,\"Blend\":0}]}");
    checkCase(unityAdd, 0.5f, L"", 0, 14, "Unity numeric Blend 1 is not equivalent to Add.");
    checkCase(unityMultiply, 0.5f, L"", 0, 12, "Unity numeric Blend 2 is not equivalent to Multiply.");
    checkCase(unityOverwrite, 0.5f, L"", 0, 16, "Unity numeric Blend 0 is not equivalent to Overwrite.");
    const auto sdkOverwrite = overwriteReference(original.modelPath, "ParamEyeLOpen");
    auto outsideTarget = base;
    outsideTarget.motionPath = files.motion(0, "ParamEyeLOpen");
    outsideTarget.expressionPathA = files.expression(L"outside-target.exp3.json", sdkOverwrite.first, "Overwrite", "ParamEyeLOpen");
    outsideTarget.expressionWeightA = 0.5f;
    auto normalizedTarget = base; normalizedTarget.motionPath = files.motion(sdkOverwrite.second, "ParamEyeLOpen");
    require(l2dae::render(outsideTarget).rgba == l2dae::render(normalizedTarget).rgba,
        "Overwrite did not clamp its target before applying half strength.");

    auto mixed = base;
    mixed.motionPathB = files.motion(24); mixed.secondsB = 0.631; mixed.blend = 0.25f;
    mixed.expressionPathA = add12; mixed.expressionWeightA = 0.5f;
    const auto mixedImage = l2dae::render(mixed);
    auto mixedReference = base; mixedReference.motionPath = files.motion(18);
    require(mixedImage.rgba == l2dae::render(mixedReference).rgba, "Expression was not applied after motion pose blending.");
    auto reordered = mixed;
    reordered.seconds = 4.56; reordered.secondsB = 0.061; reordered.expressionWeightA = 0.8125f;
    l2dae::render(reordered);
    require(mixedImage.rgba == l2dae::render(mixed).rgba, "Expression output depends on prior rendered times or strengths.");

    const auto listed = l2dae::listExpressions(original.modelPath);
    std::size_t realChanged = 0;
    if (!listed.empty()) {
        for (const auto& expression : listed) l2dae::validateExpression(expression.path);
        for (const auto& expression : listed) {
            auto real = original;
            real.expressionPathA = expression.path; real.expressionWeightA = 1;
            const auto realImage = l2dae::render(real);
            realChanged = differingPixels(realImage, baseline);
            real.expressionWeightA = 0;
            require(baseline.rgba == l2dae::render(real).rgba, "Disabling a real expression did not restore the unmodified motion.");
            if (realChanged > 0) {
                if (!previewPath.empty()) writePng(previewPath, realImage);
                std::wcout << L"Visible real expression: " << expression.label << L'\n';
                break;
            }
        }
        require(realChanged > 0, "None of the model's declared expressions produced a visible change.");
    }
    require(baseline.rgba == l2dae::render(original).rgba, "Expression cache contaminated no-expression legacy rendering.");
    l2dae::releaseRenderer();
    require(mixedImage.rgba == l2dae::render(mixed).rgba, "Expressions changed after renderer cache reset.");
    const auto reload = files.expression(L"reload.exp3.json", 12, "Add");
    checkCase(reload, 0.5f, L"", 0, 14, "Expression reload fixture failed.");
    files.write(L"reload.exp3.json", "{\"Parameters\":[{\"Id\":\"ParamAngleX\",\"Value\":24,\"Blend\":\"Add\"}]}\n\n");
    checkCase(reload, 0.5f, L"", 0, 20, "Modified expression was not reloaded from disk.");
    require(baseline.rgba == l2dae::render(original).rgba, "Expression reload changed no-expression pixels.");
    std::cout << "Expressions PASS: Add/Multiply/Overwrite, simultaneous symmetry, same-file coalescing, after-motion order, seek/reset/reload, zero/missing, "
        << malformed.size() << " invalid fixtures; all " << listed.size() << " declared expressions validate; real expression changes " << realChanged
        << " pixels; synthetic expression changes " << syntheticChanged << " pixels; Unity enum and clamped Overwrite PASS\n";
}

// Rewrite only the settings document into the test's private folder. All binary
// assets remain at their existing absolute paths; no model files are modified.
std::wstring modelWithLipGroup(const ExpressionFixtures& files, const std::wstring& original,
    const std::wstring& name, const std::string& groupIds, const std::string& eyeIds = "", bool solvers = true) {
    using namespace Live2D::Cubism::Framework;
    std::ifstream input(std::filesystem::path(original), std::ios::binary);
    const std::vector<csmByte> bytes((std::istreambuf_iterator<char>(input)), {});
    require(!bytes.empty(), "Cannot read lip-sync model settings fixture.");
    CubismModelSettingJson settings(bytes.data(), static_cast<csmSizeInt>(bytes.size()));
    const auto asset = [&](const char* relative) {
        return (std::filesystem::path(original).parent_path() / std::filesystem::u8path(relative)).generic_u8string();
    };
    std::ostringstream json;
    json << "{\"Version\":3,\"FileReferences\":{\"Moc\":" << std::quoted(asset(settings.GetModelFileName())) << ",\"Textures\":[";
    for (int i = 0; i < settings.GetTextureCount(); ++i) {
        if (i) json << ',';
        json << std::quoted(asset(settings.GetTextureFileName(i)));
    }
    json << ']';
    if (solvers && *settings.GetPhysicsFileName()) json << ",\"Physics\":" << std::quoted(asset(settings.GetPhysicsFileName()));
    if (solvers && *settings.GetPoseFileName()) json << ",\"Pose\":" << std::quoted(asset(settings.GetPoseFileName()));
    json << '}';
    if (!groupIds.empty() || !eyeIds.empty()) {
        json << ",\"Groups\":[";
        if (!groupIds.empty()) json << "{\"Target\":\"Parameter\",\"Name\":\"LipSync\",\"Ids\":[" << groupIds << "]}";
        if (!groupIds.empty() && !eyeIds.empty()) json << ',';
        if (!eyeIds.empty()) json << "{\"Target\":\"Parameter\",\"Name\":\"EyeBlink\",\"Ids\":[" << eyeIds << "]}";
        json << ']';
    }
    json << '}';
    return files.write(name, json.str());
}

void testLipSync(const l2dae::RenderRequest& original, const std::wstring& previewPath) {
    ExpressionFixtures files;
    const auto legacy = l2dae::render(original);
    auto disabled = original; disabled.mouthOpen = 1;
    require(legacy.rgba == l2dae::render(disabled).rgba, "Disabled lip sync changed legacy rendering.");
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -0.01f, 1.01f}) {
        auto request = original; request.lipSyncEnabled = true; request.mouthOpen = invalid;
        bool rejected = false;
        try { l2dae::render(request); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Invalid audio mouth opening was accepted.");
    }
    const auto mouthAndShape = [&](const std::wstring& name, float opening, float shape, bool angle = false, float angleValue = 0) {
        std::ostringstream json; json.imbue(std::locale::classic()); json << std::setprecision(9);
        json << "{\"Parameters\":[{\"Id\":\"ParamMouthOpenY\",\"Value\":" << opening << ",\"Blend\":\"Overwrite\"},"
            "{\"Id\":\"ParamMouthForm\",\"Value\":" << shape << ",\"Blend\":\"Overwrite\"}";
        if (angle) json << ",{\"Id\":\"ParamAngleX\",\"Value\":" << angleValue << ",\"Blend\":\"Overwrite\"}";
        json << "]}";
        return files.write(name, json.str());
    };
    auto driven = original;
    driven.motionPath = files.motion(0.8, "ParamMouthOpenY");
    driven.motionPathB = files.motion(0.2, "ParamMouthOpenY");
    driven.seconds = 0.73; driven.secondsB = 0.31; driven.blend = 0.375f;
    driven.expressionPathA = mouthAndShape(L"opening-expression.exp3.json", 1, 0.625f);
    driven.expressionWeightA = 1;
    driven.expressionPathB = files.expression(L"extra-opening.exp3.json", 1, "Add", "ParamMouthOpenY");
    driven.expressionWeightB = 0.5f;
    const auto withoutAudio = l2dae::render(driven);
    driven.lipSyncEnabled = true;
    std::vector<l2dae::RenderResult> openings;
    for (const float value : {0.0f, 0.4f, 1.0f}) {
        driven.mouthOpen = value;
        const auto actual = l2dae::render(driven);
        auto reference = driven;
        reference.lipSyncEnabled = false;
        reference.expressionPathA = mouthAndShape(L"reference-" + std::to_wstring(value) + L".exp3.json", value, 0.625f);
        reference.expressionWeightB = 0;
        require(actual.rgba == l2dae::render(reference).rgba,
            "Audio did not override motion/expression opening while preserving mouth shape.");
        openings.push_back(actual);
    }
    const auto changed = differingPixels(openings.front(), openings.back());
    require(changed > 0 && differingPixels(openings[0], openings[1]) > 0 && differingPixels(openings[1], openings[2]) > 0,
        "Audio mouth opening did not produce distinct closed, partial, and open poses.");
    driven.mouthOpen = 0.4f;
    auto otherShape = driven; otherShape.lipSyncEnabled = false; otherShape.expressionWeightB = 0;
    otherShape.expressionPathA = mouthAndShape(L"other-shape.exp3.json", 0.4f, -0.625f);
    require(differingPixels(openings[1], l2dae::render(otherShape)) > 0, "Mouth-shape preservation fixture is not visible.");
    auto unordered = driven;
    unordered.seconds = -2; unordered.secondsB = 5.17; unordered.mouthOpen = 0.9f;
    l2dae::render(unordered);
    require(openings[1].rgba == l2dae::render(driven).rgba, "Audio mouth opening depends on earlier rendered times or amplitudes.");
    driven.lipSyncEnabled = false;
    require(withoutAudio.rgba == l2dae::render(driven).rgba, "Disabling lip sync did not restore the motion and expressions.");
    driven.lipSyncEnabled = true;
    auto fallback = driven;
    fallback.modelPath = modelWithLipGroup(files, original.modelPath, L"no-lip-group.model3.json", "");
    require(openings[1].rgba == l2dae::render(fallback).rgba, "Exact ParamMouthOpenY fallback differs from the declared LipSync group.");
    fallback.modelPath = modelWithLipGroup(files, original.modelPath, L"invalid-lip-id.model3.json", "\"NotARealParameter\"");
    require(openings[1].rgba == l2dae::render(fallback).rgba, "An invalid LipSync ID prevented the exact-name fallback.");
    // An explicit author-selected real ID wins even when the default ID exists.
    auto explicitGroup = driven;
    explicitGroup.modelPath = modelWithLipGroup(files, original.modelPath, L"explicit-lip-group.model3.json", "\"ParamAngleX\",\"ParamAngleX\"");
    auto explicitReference = driven; explicitReference.lipSyncEnabled = false;
    explicitReference.expressionPathA = mouthAndShape(L"explicit-reference.exp3.json", 1, 0.625f, true, 0.4f);
    require(l2dae::render(explicitGroup).rgba == l2dae::render(explicitReference).rgba,
        "Declared real LipSync parameters did not take priority over the conventional fallback.");
    l2dae::releaseRenderer();
    require(openings[1].rgba == l2dae::render(driven).rgba, "Audio mouth opening changed after renderer reset.");
    require(legacy.rgba == l2dae::render(original).rgba, "Audio tests contaminated legacy rendering.");
    if (!previewPath.empty()) writePng(previewPath, openings[1]);
    std::cout << "Lip sync PASS: silence overrides mixed motion and expressions; opening changes " << changed
        << " pixels; partial opening, shape preservation, disabled restoration, seek/reset, finite range, declared IDs and exact fallback\n";
}

void testAmbient(const l2dae::RenderRequest& original, const std::wstring& previewPath) {
    using namespace Live2D::Cubism::Framework;
    ExpressionFixtures files;
    std::cout << "Ambient: validating disabled/enabled clocks and actual parameter range\n";
    const auto legacy = l2dae::render(original);
    auto disabled = original;
    disabled.ambientSeconds = std::numeric_limits<double>::quiet_NaN();
    require(legacy.rgba == l2dae::render(disabled).rgba, "Disabled ambient animation consumed its unused clock.");
    const auto breathRange = withReferenceModel(original.modelPath, [](CubismModel& model) {
        const int index = model.GetParameterIndex(CubismFramework::GetIdManager()->GetId("ParamBreath"));
        require(index < model.GetParameterCount(), "Ambient test requires the reference model's ParamBreath.");
        return std::make_pair(model.GetParameterMinimumValue(index), model.GetParameterMaximumValue(index));
    });
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto request = original; request.breathingEnabled = true; request.ambientSeconds = invalid;
        bool rejected = false;
        try { l2dae::render(request); } catch (const std::exception&) { rejected = true; }
        require(rejected, "An active ambient controller accepted a nonfinite clock.");
    }
    // Model-level failures dispose the engine/Framework. Reinitialize before
    // the fixture builder borrows Cubism's allocator and JSON implementation.
    require(legacy.rgba == l2dae::render(original).rgba, "Renderer did not recover after rejecting an ambient clock.");
    std::cout << "Ambient: building solver-free pose oracle after error recovery\n";
    const auto poseExpression = [&](const std::wstring& name, float left, float right, float angle,
        bool setBreath = false, float breath = 0) {
        std::ostringstream json; json.imbue(std::locale::classic()); json << std::setprecision(9);
        json << "{\"Parameters\":[{\"Id\":\"ParamEyeLOpen\",\"Value\":" << left << ",\"Blend\":\"Overwrite\"},"
            "{\"Id\":\"ParamEyeROpen\",\"Value\":" << right << ",\"Blend\":\"Overwrite\"},"
            "{\"Id\":\"ParamMouthOpenY\",\"Value\":1,\"Blend\":\"Overwrite\"},"
            "{\"Id\":\"ParamMouthForm\",\"Value\":0.5,\"Blend\":\"Overwrite\"},"
            "{\"Id\":\"ParamAngleX\",\"Value\":" << angle << ",\"Blend\":\"Overwrite\"}";
        if (setBreath) json << ",{\"Id\":\"ParamBreath\",\"Value\":" << breath << ",\"Blend\":\"Overwrite\"}";
        json << "]}";
        return files.write(name, json.str());
    };
    // A solver-free settings fixture makes hand-authored expression poses an
    // independent pixel oracle for controller ordering and partial blink weight.
    auto driven = original;
    driven.modelPath = modelWithLipGroup(files, original.modelPath, L"ambient.model3.json", "",
        "\"ParamEyeLOpen\",\"ParamEyeROpen\",\"ParamEyeLOpen\"", false);
    driven.motionPath = files.motion(0.8, "ParamMouthOpenY");
    driven.motionPathB = files.motion(0.2, "ParamMouthOpenY");
    driven.seconds = 0; driven.secondsB = 0; driven.blend = 0.375f;
    driven.expressionPathA = poseExpression(L"ambient-base.exp3.json", 1, 0.5f, 0);
    driven.expressionWeightA = 1;
    driven.expressionPathB = files.write(L"ambient-multiply.exp3.json",
        "{\"Parameters\":[{\"Id\":\"ParamEyeLOpen\",\"Value\":0.5,\"Blend\":\"Multiply\"},"
        "{\"Id\":\"ParamEyeROpen\",\"Value\":0.5,\"Blend\":\"Multiply\"},"
        "{\"Id\":\"ParamAngleX\",\"Value\":4,\"Blend\":\"Add\"}]}");
    driven.expressionWeightB = 0.5f;
    driven.lipSyncEnabled = true; driven.mouthOpen = 0.4f;
    const auto mixedBase = l2dae::render(driven);
    std::vector<l2dae::RenderResult> breathFrames;
    driven.breathingEnabled = true;
    for (int second = 0; second <= 2; ++second) {
        driven.ambientSeconds = second;
        const auto actual = l2dae::render(driven);
        auto reference = driven; reference.breathingEnabled = false; reference.expressionWeightB = 0;
        const float target = breathRange.first + (breathRange.second - breathRange.first) * (second * 0.5f);
        reference.expressionPathA = poseExpression(L"breath-oracle-" + std::to_wstring(second) + L".exp3.json", 0.75f, 0.375f, 2, true, target);
        require(actual.rgba == l2dae::render(reference).rgba, "Breath did not map to its actual range or changed unrelated controls.");
        breathFrames.push_back(actual);
    }
    const auto breathChanged = differingPixels(breathFrames.front(), breathFrames.back());
    require(breathChanged > 0, "Breath minimum and maximum are not visually distinct in this reference model.");
    const struct { double time, period; float amount, target; } breathingSettings[] = {
        {1, 2, 0.25f, 0.25f}, {2, 8, 0.5f, 0.25f}, {-2, 8, 0.5f, 0.25f}
    };
    for (std::size_t i = 0; i < std::size(breathingSettings); ++i) {
        const auto& setting = breathingSettings[i];
        auto adjusted = driven; adjusted.ambientSeconds = setting.time;
        adjusted.breathingAmount = setting.amount; adjusted.breathingPeriod = setting.period;
        auto reference = adjusted; reference.breathingEnabled = false; reference.expressionWeightB = 0;
        reference.expressionPathA = poseExpression(L"breath-adjusted-" + std::to_wstring(i) + L".exp3.json",
            0.75f, 0.375f, 2, true, breathRange.first + (breathRange.second - breathRange.first) * setting.target);
        require(l2dae::render(adjusted).rgba == l2dae::render(reference).rgba, "Adjusted breathing amount/period differs from pose oracle.");
    }
    auto zeroBreath = driven; zeroBreath.breathingAmount = 0;
    zeroBreath.ambientSeconds = zeroBreath.breathingPeriod = std::numeric_limits<double>::quiet_NaN();
    require(l2dae::render(zeroBreath).rgba == mixedBase.rgba, "Zero breathing amount changed the mixed pose or consumed its unused clock.");
    driven.breathingEnabled = false;
    require(mixedBase.rgba == l2dae::render(driven).rgba, "Disabling breathing did not restore the mixed pose.");
    driven.autoBlinkEnabled = true;
    l2dae::RenderResult openEyes, closedEyes;
    std::cout << "Ambient: breath range oracle passed; checking blink and closed expressions\n";
    const std::pair<double, float> samples[] = {{2.5, 1.0f}, {3.05, 0.5f}, {3.125, 0.0f}, {3.225, 0.5f}, {3.5, 1.0f}, {-0.875, 0.0f}};
    for (std::size_t i = 0; i < std::size(samples); ++i) {
        driven.ambientSeconds = samples[i].first;
        const auto actual = l2dae::render(driven);
        auto reference = driven; reference.autoBlinkEnabled = false; reference.expressionWeightB = 0;
        reference.expressionPathA = poseExpression(L"blink-oracle-" + std::to_wstring(i) + L".exp3.json",
            0.75f * samples[i].second, 0.375f * samples[i].second, 2);
        require(actual.rgba == l2dae::render(reference).rgba, "Blink did not multiply the mixed eye pose exactly once.");
        if (i == 0) openEyes = actual;
        if (i == 2) closedEyes = actual;
    }
    const auto blinkChanged = differingPixels(openEyes, closedEyes);
    require(blinkChanged > 0, "Automatic blink is not visible in the reference model.");
    const struct { double time, interval, duration; float strength, factor; } blinkSettings[] = {
        {1.625, 2, 0.75, 0.5f, 0.75f}, {1.8, 2, 0.75, 0.5f, 0.5f},
        {2.0625, 2, 0.75, 0.5f, 0.75f}, {-0.2, 2, 0.75, 0.5f, 0.5f},
        {0.23, 0.2, 2, 0.5f, 0.5f}
    };
    for (std::size_t i = 0; i < std::size(blinkSettings); ++i) {
        const auto& setting = blinkSettings[i];
        auto adjusted = driven; adjusted.ambientSeconds = setting.time;
        adjusted.blinkInterval = setting.interval; adjusted.blinkDuration = setting.duration; adjusted.blinkStrength = setting.strength;
        auto reference = adjusted; reference.autoBlinkEnabled = false; reference.expressionWeightB = 0;
        reference.expressionPathA = poseExpression(L"blink-adjusted-" + std::to_wstring(i) + L".exp3.json",
            0.75f * setting.factor, 0.375f * setting.factor, 2);
        require(l2dae::render(adjusted).rgba == l2dae::render(reference).rgba, "Adjusted blink strength/interval/duration differs from pose oracle.");
    }
    auto zeroBlink = driven; zeroBlink.blinkStrength = 0;
    zeroBlink.ambientSeconds = zeroBlink.blinkInterval = zeroBlink.blinkDuration = std::numeric_limits<double>::quiet_NaN();
    require(l2dae::render(zeroBlink).rgba == mixedBase.rgba, "Zero blink strength changed the mixed pose or consumed unused settings.");
    std::cout << "Ambient settings PASS: breathing amount/period and blink strength/interval/duration match independent pose oracles; zero strengths preserve mixed pose.\n";
    auto closedExpression = driven;
    closedExpression.expressionPathA = poseExpression(L"closed-expression.exp3.json", 0, 0, 0);
    closedExpression.ambientSeconds = 2.5;
    const auto expressionClosed = l2dae::render(closedExpression);
    for (double time : {3.05, 3.125, 3.225, 3.5}) {
        closedExpression.ambientSeconds = time;
        require(expressionClosed.rgba == l2dae::render(closedExpression).rgba, "Auto blink reopened an expression-closed eye.");
    }
    auto fallback = driven;
    fallback.modelPath = modelWithLipGroup(files, original.modelPath, L"ambient-fallback.model3.json", "", "\"MissingEye\"", false);
    require(l2dae::render(driven).rgba == l2dae::render(fallback).rgba, "Invalid EyeBlink IDs prevented exact eye-name fallback.");
    // Keep a real parameter in the declaration: only that eye should blink.
    auto declared = driven;
    declared.modelPath = modelWithLipGroup(files, original.modelPath, L"ambient-left.model3.json", "", "\"ParamEyeLOpen\",\"ParamEyeLOpen\"", false);
    declared.ambientSeconds = 3.125;
    auto declaredReference = declared; declaredReference.autoBlinkEnabled = false; declaredReference.expressionWeightB = 0;
    declaredReference.expressionPathA = poseExpression(L"left-only-reference.exp3.json", 0, 0.375f, 2);
    require(l2dae::render(declared).rgba == l2dae::render(declaredReference).rgba, "Declared eye parameters did not win or were applied twice.");
    // Reintroduce the original physics and independent clocks for seeking/cache tests.
    std::cout << "Ambient: eye oracle passed; checking original physics, independent clocks and cache reset\n";
    driven.modelPath = original.modelPath;
    driven.breathingEnabled = true; driven.ambientSeconds = 3.05;
    const auto combined = l2dae::render(driven);
    auto reordered = driven; reordered.ambientSeconds = -2.7; reordered.seconds = 8; reordered.secondsB = 3;
    l2dae::render(reordered);
    require(combined.rgba == l2dae::render(driven).rgba, "Ambient layers depend on preceding render order.");
    // Ambient time already supplies the full warmup, and both synthetic motions
    // are constant, so changing only their clocks must leave the pixels unchanged.
    reordered = driven; reordered.seconds = 12; reordered.secondsB = 19;
    require(combined.rgba == l2dae::render(reordered).rgba, "Ambient animation incorrectly follows motion-local clocks.");
    l2dae::releaseRenderer();
    require(combined.rgba == l2dae::render(driven).rgba, "Ambient layers changed after a fresh renderer cache.");
    require(legacy.rgba == l2dae::render(original).rgba, "Ambient layers contaminated disabled legacy rendering.");
    if (!previewPath.empty()) {
        const auto output = std::filesystem::path(previewPath);
        const auto save = [&](const wchar_t* suffix, const l2dae::RenderResult& frame) {
            writePng((output.parent_path() / (output.stem().wstring() + suffix + L".png")).wstring(), frame);
        };
        save(L"-breath-low", breathFrames.front()); save(L"-breath-high", breathFrames.back());
        save(L"-blink-open", openEyes); save(L"-blink-closed", closedEyes);
    }
    std::cout << "Ambient PASS: breath range changes " << breathChanged << " pixels; blink changes " << blinkChanged
        << " pixels; independent and negative clocks, partial/closed eyes, expression preservation, dual motions/expressions/audio, declared IDs/fallback, seek/reset/disabled restore\n";
}

// Optional additional SDK models exercise actual unsupported or legacy IDs.
void testAmbientCompatibility(const std::wstring& modelPath) {
    using namespace Live2D::Cubism::Framework;
    l2dae::RenderRequest request; request.modelPath = modelPath;
    request.width = request.height = 768; request.scale = 0.8f;
    const auto baseline = l2dae::render(request);
    const auto controls = withReferenceModel(modelPath, [](CubismModel& model) {
        std::pair<bool, bool> found{false, false};
        for (int i = 0; i < model.GetParameterCount(); ++i) {
            const auto id = model.GetParameterId(i);
            const auto is = [&](const char* name) { return id == CubismFramework::GetIdManager()->GetId(name); };
            found.first |= is("ParamBreath") || is("PARAM_BREATH");
            found.second |= is("ParamEyeLOpen") || is("ParamEyeROpen") || is("PARAM_EYE_L_OPEN") || is("PARAM_EYE_R_OPEN");
        }
        return found;
    });
    request.breathingEnabled = true; request.ambientSeconds = 2;
    const auto breath = l2dae::render(request);
    if (!controls.first) {
        require(baseline.rgba == breath.rgba, "A missing breath parameter changed physics history or pixels.");
        request.ambientSeconds = std::numeric_limits<double>::quiet_NaN();
        require(baseline.rgba == l2dae::render(request).rgba, "An unsupported ambient control consumed its unused clock.");
    }
    ExpressionFixtures files;
    request.breathingEnabled = false; request.autoBlinkEnabled = true; request.ambientSeconds = 3.125;
    // Remove the group so the legacy-name fallback is exercised, when present.
    request.modelPath = modelWithLipGroup(files, modelPath, L"ambient-compatible.model3.json", "");
    const auto closed = l2dae::render(request);
    request.ambientSeconds = 2.5;
    const auto opened = l2dae::render(request);
    if (controls.second) require(differingPixels(closed, opened) > 0, "Known eye aliases produced no visible blink.");
    else require(closed.rgba == opened.rgba, "A model without known eye openings changed under auto blink.");
    request.autoBlinkEnabled = false;
    require(baseline.rgba == l2dae::render(request).rgba, "Disabling ambient fallback did not restore the original model.");
    std::cout << "Ambient compatibility PASS: breath supported=" << controls.first << ", eye aliases supported=" << controls.second
        << ", fallback blink pixels=" << differingPixels(closed, opened) << '\n';
    l2dae::releaseRenderer();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    if (argc == 3 && std::wstring(argv[1]) == L"--ambient-compat") {
        try { testAmbientCompatibility(std::filesystem::absolute(argv[2]).wstring()); return 0; }
        catch (const std::exception& error) { l2dae::releaseRenderer(); std::cerr << error.what() << '\n'; return 1; }
    }
    if (argc < 2 || argc > 4) {
        std::wcerr << L"Usage: Live2DRenderProbe.exe <model3.json> [motion3.json|-] [output.png]\n";
        return 2;
    }
    try {
        l2dae::RenderRequest request;
        request.modelPath = std::filesystem::absolute(argv[1]).wstring();
        request.width = request.height = 768;
        request.scale = 0.8f; // Leave room for the translation acceptance test.
        const auto motions = l2dae::listMotions(request.modelPath);
        std::cout << "Motion entries: " << motions.size() << '\n';
        if (!motions.empty()) {
            const auto duration = l2dae::motionDuration(motions.front().path);
            require(std::isfinite(duration) && duration > 0 && duration <= 86400, "Native motion duration is invalid.");
            std::cout << "Import clip duration: " << duration << " seconds\n";
        }
        if (argc >= 3 && std::wstring(argv[2]) != L"-") request.motionPath = std::filesystem::absolute(argv[2]).wstring();
        else if (argc == 2 && !motions.empty()) request.motionPath = motions.front().path;

        auto invalid = request;
        invalid.width = 0;
        bool rejected = false;
        try { l2dae::render(invalid); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Zero-width render was not rejected.");
        invalid = request;
        invalid.seconds = std::numeric_limits<double>::quiet_NaN();
        rejected = false;
        try { l2dae::render(invalid); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Non-finite motion time was not rejected.");
        for (int invalidCase = 0; invalidCase < 4; ++invalidCase) {
            invalid = request;
            if (invalidCase == 0) invalid.secondsB = std::numeric_limits<double>::infinity();
            if (invalidCase == 1) invalid.blend = std::numeric_limits<float>::quiet_NaN();
            if (invalidCase == 2) invalid.blend = -0.01f;
            if (invalidCase == 3) invalid.blend = 1.1001f; // Up to 1.1 is valid pose overshoot.
            rejected = false;
            try { l2dae::render(invalid); } catch (const std::exception&) { rejected = true; }
            require(rejected, "Invalid B clock or blend was not rejected.");
        }

        request.seconds = 3.371;
        const auto begin = std::chrono::steady_clock::now();
        const auto first = l2dae::render(request);
        const auto firstBounds = alphaBounds(first);
        require(first.rgba.size() == 768u * 768u * 4u, "Wrong readback dimensions.");
        require(firstBounds.pixels > 100, "Model render is transparent or empty.");
        request.seconds = 5.219;
        const auto later = l2dae::render(request);
        request.seconds = 0.118;
        l2dae::render(request);
        request.seconds = 3.371;
        const auto revisited = l2dae::render(request);
        require(first.rgba == revisited.rgba, "Out-of-order seeking produced different pixels for the same time.");
        const auto changed = differingPixels(first, later);
        std::cout << "Out-of-order seek: identical pixels; motion change: " << changed << " pixels\n";

        request.offsetX = 24;
        request.offsetY = 18;
        const auto shifted = alphaBounds(l2dae::render(request));
        require(std::abs((shifted.left - firstBounds.left) - 24) <= 2 &&
            std::abs((shifted.top - firstBounds.top) - 18) <= 2,
            "Model translation direction or pixel units are incorrect.");
        request.offsetX = request.offsetY = 0;
        request.pixelAspect = 2;
        const auto widePixels = alphaBounds(l2dae::render(request));
        require(widePixels.pixels > 100, "Non-square pixel output is empty.");
        const double normalWidth = firstBounds.right - firstBounds.left + 1;
        const double normalHeight = firstBounds.bottom - firstBounds.top + 1;
        const double physicalWidth = 2.0 * (widePixels.right - widePixels.left + 1);
        const double physicalHeight = widePixels.bottom - widePixels.top + 1;
        require(std::abs((physicalWidth / physicalHeight) / (normalWidth / normalHeight) - 1) < 0.035,
            "Non-square pixels distort the model.");
        request.pixelAspect = 1;
        l2dae::releaseRenderer();
        const auto reopened = l2dae::render(request);
        require(first.rgba == reopened.rgba, "Fresh renderer produced different pixels for the same time.");
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::cout << "Alpha pixels: " << firstBounds.pixels << "; 7 render calls and 2 initializations: " << elapsed << " s\n";

        auto transition = request;
        transition.motionPathB = motions.empty() ? request.motionPath : motions.back().path;
        transition.secondsB = 0.734;
        transition.blend = 1;
        auto onlyB = request;
        onlyB.motionPath = transition.motionPathB;
        onlyB.seconds = transition.secondsB;
        const auto endpointB = l2dae::render(onlyB);
        const auto bSelected = l2dae::render(transition);
        require(endpointB.rgba == bSelected.rgba, "Blend 1 does not match B at B's own local time.");
        transition.blend = 0;
        require(first.rgba == l2dae::render(transition).rgba, "Blend 0 does not match source A.");
        const std::wstring missingMotion = request.modelPath + L".missing.motion3.json";
        auto inactive = transition;
        inactive.motionPathB = missingMotion;
        require(first.rgba == l2dae::render(inactive).rgba, "Inactive B file was loaded at blend 0.");
        inactive = transition;
        inactive.motionPath = missingMotion;
        inactive.blend = 1;
        require(endpointB.rgba == l2dae::render(inactive).rgba, "Inactive A file was loaded at blend 1.");

        transition.blend = 0.375f;
        const auto interior = l2dae::render(transition);
        auto swapped = transition;
        std::swap(swapped.motionPath, swapped.motionPathB);
        std::swap(swapped.seconds, swapped.secondsB);
        swapped.blend = 0.625f;
        require(interior.rgba == l2dae::render(swapped).rgba, "Swapping A/B and complementing the blend changed the pose.");
        auto backwards = transition;
        backwards.seconds = 0.061;
        backwards.secondsB = 5.392;
        backwards.blend = 0.8125f;
        const auto backwardsResult = l2dae::render(backwards);
        require(interior.rgba == l2dae::render(transition).rgba, "A/B backward time remapping depends on render order.");
        require(backwardsResult.rgba == l2dae::render(backwards).rgba, "Repeated backward local clocks changed the output.");

        auto sameSource = transition;
        sameSource.motionPathB = sameSource.motionPath;
        sameSource.secondsB = sameSource.seconds;
        require(first.rgba == l2dae::render(sameSource).rgba, "Blending identical source/time altered the pose.");
        sameSource.secondsB = 0.319;
        const auto sameFileDifferentTimes = l2dae::render(sameSource);
        auto sameFileSwapped = sameSource;
        std::swap(sameFileSwapped.seconds, sameFileSwapped.secondsB);
        sameFileSwapped.blend = 0.625f;
        require(sameFileDifferentTimes.rgba == l2dae::render(sameFileSwapped).rgba,
            "Using one cached motion at two clocks corrupted a source.");

        auto staticPose = request;
        staticPose.motionPath.clear();
        staticPose.seconds = transition.secondsB;
        const auto staticFrame = l2dae::render(staticPose);
        auto staticB = transition;
        staticB.motionPathB.clear();
        staticB.blend = 1;
        require(staticFrame.rgba == l2dae::render(staticB).rgba, "Empty B does not select the static model pose.");
        staticB.blend = 0.375f;
        const auto toStatic = l2dae::render(staticB);
        auto staticA = staticB;
        std::swap(staticA.motionPath, staticA.motionPathB);
        std::swap(staticA.seconds, staticA.secondsB);
        staticA.blend = 0.625f;
        require(toStatic.rgba == l2dae::render(staticA).rgba, "Static/motion blending is not symmetric.");

        const auto endpointDifference = differingPixels(first, endpointB);
        if (endpointDifference > 100) {
            require(differingPixels(first, interior) > 0 && differingPixels(endpointB, interior) > 0,
                "Interior transition did not produce a distinct pose.");
            std::size_t unlikeDissolve = 0;
            for (std::size_t i = 0; i < interior.rgba.size(); ++i) {
                const int dissolve = static_cast<int>(std::lround(first.rgba[i] * 0.625 + endpointB.rgba[i] * 0.375));
                if (std::abs(static_cast<int>(interior.rgba[i]) - dissolve) > 1) ++unlikeDissolve;
            }
            require(unlikeDissolve > 64, "Transition appears to dissolve two rendered images instead of blending model poses.");
            std::cout << "Pose blend: " << endpointDifference << " endpoint pixels differ; " << unlikeDissolve
                << " interior channels differ from image dissolve\n";
        }
        l2dae::releaseRenderer();
        require(interior.rgba == l2dae::render(transition).rgba, "Blend output changed after clearing the renderer cache.");
        std::cout << "Transitions: endpoints, source symmetry, unordered independent clocks, static pose, inactive paths, shared motion and fresh cache PASS\n";
        std::wstring expressionPreview;
        if (argc >= 4) {
            const auto output = std::filesystem::absolute(argv[3]);
            expressionPreview = (output.parent_path() / (output.stem().wstring() + L"-applied.png")).wstring();
        }
        testExpressions(request, expressionPreview);
        std::wstring lipSyncPreview;
        if (argc >= 4) {
            const auto output = std::filesystem::absolute(argv[3]);
            lipSyncPreview = (output.parent_path() / (output.stem().wstring() + L"-lip-sync.png")).wstring();
        }
        testLipSync(request, lipSyncPreview);
        testAmbient(request, argc >= 4 ? std::filesystem::absolute(argv[3]).wstring() : L"");
        const auto warmed = std::chrono::steady_clock::now();
        for (int i = 0; i < 12; ++i) {
            request.seconds = 3.0 + i / 60.0;
            l2dae::render(request);
        }
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warmed).count() / 12.0;
        std::cout << "Warm rendering, 768x768, 12 frames: " << milliseconds << " ms/frame\n";
        if (argc >= 4) {
            writePng(std::filesystem::absolute(argv[3]).wstring(), first);
            std::wcout << L"Preview: " << argv[3] << L'\n';
        }
        l2dae::releaseRenderer();
        std::cout << "PASS: render, input validation, seek determinism, translation, pixel aspect, pose transitions, expression overlays, audio mouth opening, breathing/auto blink, cache reset.\n";
        return 0;
    } catch (const std::exception& error) {
        l2dae::releaseRenderer();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
