// Compare fresh CubismPhysics::Create with the build-only ResetForEvaluation
// extension using real MOC parameters. No private state or object-layout access.
// Usage: Live2DPhysicsResetTest <SDK Samples/Resources directory | model3.json>
// Link the patched Framework target (which already links CubismCore/D3D11).
#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
#include <ICubismAllocator.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <Physics/CubismPhysics.hpp>
#include <algorithm>
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
    unsigned long long comparisons = 0;
    unsigned long long resets = 0;
    unsigned int models = 0;
};

std::vector<Csm::csmByte> readBytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + path.u8string());
    std::vector<Csm::csmByte> bytes(std::istreambuf_iterator<char>(stream), {});
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Empty or oversized test fixture: " + path.u8string());
    return bytes;
}

void sampleInput(Csm::CubismModel* model, double phase) {
    for (int i = 0; i < model->GetParameterCount(); ++i) {
        const double minimum = model->GetParameterMinimumValue(i);
        const double maximum = model->GetParameterMaximumValue(i);
        const double fraction = 0.5 + 0.49 * std::sin(phase + static_cast<double>(i) * 0.319);
        model->SetParameterValue(i, static_cast<float>(minimum + (maximum - minimum) * fraction));
    }
}

void compare(Csm::CubismModel* fresh, Csm::CubismModel* reused,
    const std::string& name, int trial, int step, Counts& counts) {
    for (int i = 0; i < fresh->GetParameterCount(); ++i) {
        const float expected = fresh->GetParameterValue(i);
        const float actual = reused->GetParameterValue(i);
        ++counts.comparisons;
        // Compare the public floating-point values bit for bit, including zero
        // signs; no epsilon can conceal history-dependent rounding differences.
        if (std::memcmp(&expected, &actual, sizeof(float)) != 0) {
            std::printf("FAIL %s trial=%d step=%d parameter=%d fresh=%.9g reused=%.9g\n",
                name.c_str(), trial, step, i, expected, actual);
            throw std::runtime_error("Physics parameter bits differ after reset.");
        }
    }
}

void runModel(const fs::path& modelPath, Allocator& allocator, Counts& counts) {
    const auto settingsBytes = readBytes(modelPath);
    Csm::CubismModelSettingJson settings(settingsBytes.data(), static_cast<int>(settingsBytes.size()));
    if (!settings.IsValid()) throw std::runtime_error("Invalid model3.json: " + modelPath.u8string());
    const std::string physicsFile = settings.GetPhysicsFileName();
    if (physicsFile.empty()) {
        std::printf("SKIP %s (no physics)\n", modelPath.u8string().c_str());
        return;
    }
    const std::string name = modelPath.filename().u8string();
    const auto mocBytes = readBytes(modelPath.parent_path() / fs::u8path(settings.GetModelFileName()));
    const auto physicsBytes = readBytes(modelPath.parent_path() / fs::u8path(physicsFile));
    std::unique_ptr<Csm::CubismMoc, decltype(&Csm::CubismMoc::Delete)> moc(
        Csm::CubismMoc::Create(mocBytes.data(), static_cast<int>(mocBytes.size()), true), Csm::CubismMoc::Delete);
    if (!moc) throw std::runtime_error("Cannot create MOC: " + name);
    const auto deleteModel = [&](Csm::CubismModel* model) { if (model) moc->DeleteModel(model); };
    std::unique_ptr<Csm::CubismModel, decltype(deleteModel)> freshModel(moc->CreateModel(), deleteModel);
    std::unique_ptr<Csm::CubismModel, decltype(deleteModel)> reusedModel(moc->CreateModel(), deleteModel);
    if (!freshModel || !reusedModel) throw std::runtime_error("Cannot create model: " + name);
    using Physics = std::unique_ptr<Csm::CubismPhysics, decltype(&Csm::CubismPhysics::Delete)>;
    const auto createPhysics = [&]() {
        Physics physics(Csm::CubismPhysics::Create(physicsBytes.data(), static_cast<int>(physicsBytes.size())),
            Csm::CubismPhysics::Delete);
        if (!physics) throw std::runtime_error("Cannot create physics: " + name);
        return physics;
    };
    auto reused = createPhysics();
    const float deltas[]{0.0f, -0.0217f, 0.00001f, 0.0137f, 1.0f / 30.0f, 0.05123f, 0.22f, 5.01f};
    for (int trial = 0; trial < 256; ++trial) {
        // Deliberately pollute parameters, cached outputs, options, particles and
        // the fractional clock with a different history before each new seek.
        Csm::CubismPhysics::Options dirty;
        dirty.Gravity = Csm::CubismVector2(0.31f, -0.76f);
        dirty.Wind = Csm::CubismVector2(0.2f, -0.13f);
        reused->SetOptions(dirty);
        sampleInput(reusedModel.get(), trial * 0.7 - 17.1);
        reused->Stabilization(reusedModel.get());
        for (int j = 0; j < 1 + trial % 11; ++j) {
            sampleInput(reusedModel.get(), trial * 0.3 + j * 0.21);
            reused->Evaluate(reusedModel.get(), 0.01731f + j * 0.00073f);
        }
        if (trial % 3 == 0) reused->Reset(); // Also undo official Reset's rig force changes.
        auto fresh = createPhysics();
        const auto allocationsBefore = allocator.allocations;
        reused->ResetForEvaluation();
        if (allocationsBefore != allocator.allocations)
            throw std::runtime_error("ResetForEvaluation allocated memory.");
        ++counts.resets;
        const auto& options = reused->GetOptions();
        if (options.Gravity.X != 0 || options.Gravity.Y != -1 || options.Wind.X != 0 || options.Wind.Y != 0)
            throw std::runtime_error("ResetForEvaluation did not restore default options.");
        if (trial % 4 == 1) {
            // Both new and reused objects must still accept custom force options.
            fresh->SetOptions(dirty);
            reused->SetOptions(dirty);
        }
        const double phase = (trial % 2 ? -1.0 : 1.0) * (trial * 0.213 + 0.0119);
        sampleInput(freshModel.get(), phase);
        sampleInput(reusedModel.get(), phase);
        // Every eighth trial directly evaluates without Stabilization, a stronger
        // check of Create-state restoration than the renderer's contract requires.
        if (trial % 8 != 0) {
            fresh->Stabilization(freshModel.get());
            reused->Stabilization(reusedModel.get());
        }
        compare(freshModel.get(), reusedModel.get(), name, trial, -1, counts);
        const int steps = trial % 13; // Includes zero warmup and no Evaluate calls.
        for (int j = 0; j < steps; ++j) {
            sampleInput(freshModel.get(), phase + j * 0.113);
            sampleInput(reusedModel.get(), phase + j * 0.113);
            const float delta = deltas[(trial + j) % 8];
            fresh->Evaluate(freshModel.get(), delta);
            reused->Evaluate(reusedModel.get(), delta);
            compare(freshModel.get(), reusedModel.get(), name, trial, j, counts);
        }
    }
    ++counts.models;
    std::printf("PASS %s 256 reset histories\n", name.c_str());
}

std::vector<fs::path> findModels(const fs::path& input) {
    if (!fs::is_directory(input)) return {input};
    std::vector<fs::path> models;
    for (const auto& entry : fs::recursive_directory_iterator(input)) {
        if (!entry.is_regular_file()) continue;
        const auto filename = entry.path().filename().wstring();
        const std::wstring suffix = L".model3.json";
        if (filename.size() >= suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0)
            models.push_back(entry.path());
    }
    std::sort(models.begin(), models.end());
    return models;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: Live2DPhysicsResetTest <SDK Samples/Resources directory | model3.json>\n");
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
        if (counts.models == 0) throw std::runtime_error("No model with physics was tested.");
        std::printf("PASS models=%u resets=%llu float-bit comparisons=%llu zero reset allocations\n",
            counts.models, counts.resets, counts.comparisons);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR %s\n", error.what());
        result = 1;
    }
    Csm::CubismFramework::Dispose();
    Csm::CubismFramework::CleanUp();
    return result;
}
