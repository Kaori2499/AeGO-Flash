// Validate the build-only Core reset extension against truly fresh models.
// Uses only public Framework/Core APIs and official SDK sample assets.
// Usage: Live2DCoreResetTest <SDK Samples/Resources directory | model3.json>
#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
#include <ICubismAllocator.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <malloc.h>

namespace Csm = Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;
namespace fs = std::filesystem;

namespace {
class Allocator final : public Csm::ICubismAllocator {
public:
    unsigned long long allocations = 0;
    void* Allocate(Csm::csmSizeType size) override { ++allocations; return std::malloc(size); }
    void Deallocate(void* memory) override { std::free(memory); }
    void* AllocateAligned(Csm::csmSizeType size, Csm::csmUint32 alignment) override {
        ++allocations;
        return _aligned_malloc(size, alignment);
    }
    void DeallocateAligned(void* memory) override { _aligned_free(memory); }
};

struct Counts {
    unsigned long long arrays = 0;
    unsigned long long comparedBytes = 0;
    unsigned long long resets = 0;
    unsigned long long rejected = 0;
    unsigned int models = 0;
};

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<Csm::csmByte> readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.u8string());
    std::vector<Csm::csmByte> bytes(std::istreambuf_iterator<char>(input), {});
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Empty or oversized fixture: " + path.u8string());
    return bytes;
}

template<class T>
void compareArray(const T* expected, const T* actual, std::size_t length,
    const std::string& name, int trial, unsigned slot, const char* field, Counts& counts) {
    const std::size_t bytes = length * sizeof(T);
    ++counts.arrays;
    counts.comparedBytes += bytes;
    if (bytes == 0) return;
    require(expected && actual, "Core returned a missing nonempty array.");
    if (std::memcmp(expected, actual, bytes) != 0) {
        const auto* a = reinterpret_cast<const unsigned char*>(expected);
        const auto* b = reinterpret_cast<const unsigned char*>(actual);
        std::size_t first = 0;
        while (first < bytes && a[first] == b[first]) ++first;
        std::printf("FAIL %s trial=%d slot=%u field=%s first_byte=%zu expected=%u actual=%u\n",
            name.c_str(), trial, slot, field, first,
            static_cast<unsigned>(a[first]), static_cast<unsigned>(b[first]));
        throw std::runtime_error("Core reset output is not bitwise identical to a fresh model.");
    }
}

struct Identity {
    Core::csmModel* core = nullptr;
    std::array<int, 4> counts{};
    std::array<const void*, 20> arrays{};
    std::vector<const void*> vertices;
    std::vector<Csm::CubismIdHandle> ids;
};

Identity identity(Csm::CubismModel* model) {
    Identity result;
    result.core = model->GetModel();
    result.counts = {model->GetParameterCount(), model->GetPartCount(),
        model->GetDrawableCount(), model->GetOffscreenCount()};
    auto* core = result.core;
    result.arrays = {
        Core::csmGetParameterIds(core), Core::csmGetParameterMinimumValues(core),
        Core::csmGetParameterMaximumValues(core), Core::csmGetParameterDefaultValues(core),
        Core::csmGetParameterValues(core), Core::csmGetParameterTypes(core),
        Core::csmGetPartIds(core), Core::csmGetPartOpacities(core),
        Core::csmGetDrawableIds(core), Core::csmGetDrawableVertexCounts(core),
        Core::csmGetDrawableVertexPositions(core), Core::csmGetDrawableVertexUvs(core),
        Core::csmGetDrawableIndices(core), Core::csmGetDrawableTextureIndices(core),
        Core::csmGetDrawableOpacities(core), Core::csmGetDrawableMultiplyColors(core),
        Core::csmGetDrawableScreenColors(core), Core::csmGetDrawableMaskCounts(core),
        Core::csmGetDrawableMasks(core), Core::csmGetRenderOrders(core)};
    for (int i = 0; i < result.counts[0]; ++i) result.ids.push_back(model->GetParameterId(i));
    for (int i = 0; i < result.counts[1]; ++i) result.ids.push_back(model->GetPartId(i));
    for (int i = 0; i < result.counts[2]; ++i) {
        result.ids.push_back(model->GetDrawableId(i));
        result.vertices.push_back(model->GetDrawableVertexPositions(i));
    }
    return result;
}

void sameIdentity(const Identity& expected, Csm::CubismModel* model) {
    const auto actual = identity(model);
    require(expected.core == actual.core && expected.counts == actual.counts &&
        expected.arrays == actual.arrays && expected.vertices == actual.vertices && expected.ids == actual.ids,
        "Core reset invalidated a model pointer, public array, count, or Framework ID.");
}

void sampleInput(Csm::CubismModel* model, double phase) {
    for (int i = 0; i < model->GetParameterCount(); ++i) {
        const double minimum = model->GetParameterMinimumValue(i);
        const double maximum = model->GetParameterMaximumValue(i);
        const double fraction = 0.5 + 0.493 * std::sin(phase + static_cast<double>(i) * 0.319);
        model->SetParameterValue(i, static_cast<float>(minimum + (maximum - minimum) * fraction));
    }
    for (int i = 0; i < model->GetPartCount(); ++i)
        model->SetPartOpacity(i, static_cast<float>(0.5 + 0.49 * std::sin(phase * 0.719 + i * 0.413)));
    model->SetModelOpacity(static_cast<float>(0.5 + 0.49 * std::sin(phase * 0.17)));
}

void compare(Csm::CubismModel* fresh, Csm::CubismModel* reused,
    const std::string& name, int trial, unsigned slot, Counts& counts) {
    require(fresh->GetParameterCount() == reused->GetParameterCount() &&
        fresh->GetPartCount() == reused->GetPartCount() &&
        fresh->GetDrawableCount() == reused->GetDrawableCount() &&
        fresh->GetOffscreenCount() == reused->GetOffscreenCount(), "Fresh/reused Core array counts differ.");
    auto* expected = fresh->GetModel();
    auto* actual = reused->GetModel();
    const auto params = static_cast<std::size_t>(fresh->GetParameterCount());
    const auto parts = static_cast<std::size_t>(fresh->GetPartCount());
    const auto drawables = static_cast<std::size_t>(fresh->GetDrawableCount());
    const auto offscreens = static_cast<std::size_t>(fresh->GetOffscreenCount());
    const auto array = [&](const auto* a, const auto* b, std::size_t length, const char* field) {
        compareArray(a, b, length, name, trial, slot, field, counts);
    };
    array(Core::csmGetParameterValues(expected), Core::csmGetParameterValues(actual), params, "parameters");
    array(Core::csmGetPartOpacities(expected), Core::csmGetPartOpacities(actual), parts, "parts");
    array(Core::csmGetDrawableOpacities(expected), Core::csmGetDrawableOpacities(actual), drawables, "drawable-opacity");
    array(Core::csmGetDrawableMultiplyColors(expected), Core::csmGetDrawableMultiplyColors(actual), drawables, "multiply-colors");
    array(Core::csmGetDrawableScreenColors(expected), Core::csmGetDrawableScreenColors(actual), drawables, "screen-colors");
    array(Core::csmGetDrawableDrawOrders(expected), Core::csmGetDrawableDrawOrders(actual), drawables, "draw-order");
    array(Core::csmGetRenderOrders(expected), Core::csmGetRenderOrders(actual), drawables + offscreens, "render-order");
    array(Core::csmGetDrawableDynamicFlags(expected), Core::csmGetDrawableDynamicFlags(actual), drawables, "dynamic-flags");
    array(Core::csmGetOffscreenOpacities(expected), Core::csmGetOffscreenOpacities(actual), offscreens, "offscreen-opacity");
    array(Core::csmGetOffscreenMultiplyColors(expected), Core::csmGetOffscreenMultiplyColors(actual), offscreens, "offscreen-multiply");
    array(Core::csmGetOffscreenScreenColors(expected), Core::csmGetOffscreenScreenColors(actual), offscreens, "offscreen-screen");
    for (int i = 0; i < fresh->GetDrawableCount(); ++i) {
        require(fresh->GetDrawableVertexCount(i) == reused->GetDrawableVertexCount(i), "Drawable vertex counts differ.");
        const std::string field = "vertices-" + std::to_string(i);
        array(fresh->GetDrawableVertexPositions(i), reused->GetDrawableVertexPositions(i),
            static_cast<std::size_t>(fresh->GetDrawableVertexCount(i)), field.c_str());
    }
    // These access the Framework's cached pointers refreshed by the extension.
    for (int i = 0; i < fresh->GetParameterCount(); ++i) {
        const float a = fresh->GetParameterValue(i), b = reused->GetParameterValue(i);
        array(&a, &b, 1, "framework-parameter");
    }
    for (int i = 0; i < fresh->GetPartCount(); ++i) {
        const float a = fresh->GetPartOpacity(i), b = reused->GetPartOpacity(i);
        array(&a, &b, 1, "framework-part");
    }
    const float opacityA = fresh->GetModelOpacity(), opacityB = reused->GetModelOpacity();
    array(&opacityA, &opacityB, 1, "framework-model-opacity");
}

void runModel(const fs::path& path, Allocator& allocator, Counts& counts) {
    const auto settingBytes = readBytes(path);
    Csm::CubismModelSettingJson settings(settingBytes.data(), static_cast<int>(settingBytes.size()));
    require(settings.IsValid(), "Invalid model settings.");
    const auto mocBytes = readBytes(path.parent_path() / fs::u8path(settings.GetModelFileName()));
    const std::string name = path.filename().u8string();
    using Moc = std::unique_ptr<Csm::CubismMoc, decltype(&Csm::CubismMoc::Delete)>;
    const auto createMoc = [&]() {
        Moc value(Csm::CubismMoc::Create(mocBytes.data(), static_cast<int>(mocBytes.size()), true), Csm::CubismMoc::Delete);
        require(value != nullptr, "Cannot create sample MOC.");
        return value;
    };
    auto moc = createMoc();
    auto wrongMoc = createMoc(); // Same bytes but a different owner must still fail.
    const auto deleteModel = [&](Csm::CubismModel* model) { if (model) moc->DeleteModel(model); };
    using Model = std::unique_ptr<Csm::CubismModel, decltype(deleteModel)>;
    const auto createModel = [&]() {
        Model value(moc->CreateModel(), deleteModel);
        require(value != nullptr, "Cannot create sample model.");
        return value;
    };
    std::array<Model, 2> reused{createModel(), createModel()};
    const std::array<Identity, 2> original{identity(reused[0].get()), identity(reused[1].get())};
    const auto virtualId = Csm::CubismFramework::GetIdManager()->GetId("CodexCoreResetVirtualParameter");
    const std::array<int, 2> virtualIndices{
        reused[0]->GetParameterIndex(virtualId), reused[1]->GetParameterIndex(virtualId)};
    require(virtualIndices[0] >= reused[0]->GetParameterCount() && virtualIndices[1] >= reused[1]->GetParameterCount(),
        "Synthetic virtual parameter unexpectedly exists in the MOC.");
    auto allocationsBefore = allocator.allocations;
    require(!moc->ResetModelForEvaluation(nullptr), "Null reset was accepted.");
    require(!wrongMoc->ResetModelForEvaluation(reused[0].get()), "Reset accepted a model belonging to another MOC.");
    require(allocator.allocations == allocationsBefore, "Rejected reset allocated Framework memory.");
    counts.rejected += 2;

    for (int trial = 0; trial < 64; ++trial) {
        // Two live models share the MOC but must never share mutable Core state.
        // Each comparison model is newly allocated and evaluated exactly once.
        std::array<Model, 2> fresh{createModel(), createModel()};
        for (int dirtyStep = 0; dirtyStep < 1 + trial % 7; ++dirtyStep)
            for (unsigned slot = 0; slot < 2; ++slot) {
                sampleInput(reused[slot].get(), trial * 0.179 + dirtyStep * 0.327 + slot * 0.813 - 18.31);
                reused[slot]->Update();
            }
        for (unsigned slot = 0; slot < 2; ++slot) {
            const float virtualValue = 0.137f + slot * 0.219f;
            reused[slot]->SetParameterValue(virtualIndices[slot], virtualValue);
            allocationsBefore = allocator.allocations;
            const bool reset = moc->ResetModelForEvaluation(reused[slot].get());
            require(allocator.allocations == allocationsBefore, "Reset allocated Framework memory.");
            require(reset, "Valid model reset was rejected.");
            ++counts.resets;
            sameIdentity(original[slot], reused[slot].get());
            const float afterVirtual = reused[slot]->GetParameterValue(virtualIndices[slot]);
            require(std::memcmp(&virtualValue, &afterVirtual, sizeof(float)) == 0,
                "Core reset replaced Framework virtual parameter storage.");
            const double phase = (trial % 2 ? -1.0 : 1.0) * (trial * 0.193 + slot * 0.271 + 0.0197);
            sampleInput(fresh[slot].get(), phase);
            sampleInput(reused[slot].get(), phase);
            fresh[slot]->Update();
            reused[slot]->Update();
            compare(fresh[slot].get(), reused[slot].get(), name, trial, slot, counts);
        }
        // Resetting/evaluating slot 1 must leave the completed slot 0 untouched.
        compare(fresh[0].get(), reused[0].get(), name, trial, 0, counts);
        allocationsBefore = allocator.allocations;
        require(!wrongMoc->ResetModelForEvaluation(reused[1].get()), "Wrong-owner reset was accepted.");
        require(allocator.allocations == allocationsBefore, "Wrong-owner reset allocated Framework memory.");
        ++counts.rejected;
        compare(fresh[1].get(), reused[1].get(), name, trial, 1, counts);
    }
    ++counts.models;
    std::printf("PASS %s 64 histories x 2 interleaved models; Core buffers/Framework IDs retained\n", name.c_str());
}

std::vector<fs::path> findModels(const fs::path& input) {
    if (!fs::is_directory(input)) return {input};
    std::vector<fs::path> models;
    const std::wstring suffix = L".model3.json";
    for (const auto& entry : fs::recursive_directory_iterator(input)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().wstring();
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            models.push_back(entry.path());
    }
    std::sort(models.begin(), models.end());
    return models;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: Live2DCoreResetTest <SDK Samples/Resources directory | model3.json>\n");
        return 2;
    }
    Allocator allocator;
    Csm::CubismFramework::Option options{};
    options.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Off;
    if (!Csm::CubismFramework::StartUp(&allocator, &options)) return 3;
    Csm::CubismFramework::Initialize();
    int result = 0;
    try {
        Counts counts;
        for (const auto& model : findModels(fs::path(argv[1]))) runModel(model, allocator, counts);
        require(counts.models > 0, "No SDK sample model was tested.");
        std::printf("PASS models=%u resets=%llu rejected=%llu bitwise_arrays=%llu bytes=%llu zero reset allocator calls\n",
            counts.models, counts.resets, counts.rejected, counts.arrays, counts.comparedBytes);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR %s\n", error.what());
        result = 1;
    }
    Csm::CubismFramework::Dispose();
    Csm::CubismFramework::CleanUp();
    return result;
}
