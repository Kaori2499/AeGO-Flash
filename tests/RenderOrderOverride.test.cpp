// Framework-owned render-order permutations must never alter Core data or
// survive a frame reset. Usage: Live2DRenderOrderOverrideTest model3.json
#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
#include <ICubismAllocator.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
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
        ++allocations; return _aligned_malloc(size, alignment);
    }
    void DeallocateAligned(void* memory) override { _aligned_free(memory); }
};
unsigned checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
std::vector<Csm::csmByte> read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path.u8string());
    return {std::istreambuf_iterator<char>(file), {}};
}
void equal(const Csm::csmInt32* actual, const std::vector<Csm::csmInt32>& expected, const char* message) {
    require(actual && std::equal(expected.begin(), expected.end(), actual), message);
}
struct MocDeleter { void operator()(Csm::CubismMoc* moc) const { Csm::CubismMoc::Delete(moc); } };
struct ModelDeleter {
    Csm::CubismMoc* moc = nullptr;
    void operator()(Csm::CubismModel* model) const { if (model) moc->DeleteModel(model); }
};
void run(const fs::path& modelPath, Allocator& allocator) {
    const auto settingsBytes = read(modelPath);
    Csm::CubismModelSettingJson settings(settingsBytes.data(), static_cast<int>(settingsBytes.size()));
    const auto mocBytes = read(modelPath.parent_path() / fs::u8path(settings.GetModelFileName()));
    std::unique_ptr<Csm::CubismMoc, MocDeleter> moc(Csm::CubismMoc::Create(
        mocBytes.data(), static_cast<int>(mocBytes.size()), true));
    require(static_cast<bool>(moc), "MOC creation failed.");
    std::unique_ptr<Csm::CubismModel, ModelDeleter> model(moc->CreateModel(), ModelDeleter{moc.get()});
    std::unique_ptr<Csm::CubismModel, ModelDeleter> other(moc->CreateModel(), ModelDeleter{moc.get()});
    require(model && other, "Model creation failed.");
    const int count = model->GetDrawableCount() + model->GetOffscreenCount();
    require(count > 1, "Fixture must have at least two render objects.");
    model->Update(); other->Update();
    auto* const core = model->GetModel();
    const auto* const coreOrders = Core::csmGetRenderOrders(core);
    const std::vector<Csm::csmInt32> native(coreOrders, coreOrders + count);
    require(model->GetRenderOrders() == coreOrders, "Fresh model does not expose Core orders.");
    std::vector<Csm::csmInt32> a(count), b(count);
    std::iota(a.begin(), a.end(), 0);
    std::reverse(a.begin(), a.end());
    std::iota(b.begin(), b.end(), 0);
    std::rotate(b.begin(), b.begin() + count / 2, b.end());
    require(model->SetRenderOrderOverrideForEvaluation(a.data(), count), "Permutation A rejected.");
    equal(model->GetRenderOrders(), a, "A not exposed.");
    require(model->GetRenderOrders() != coreOrders && model->GetRenderOrders() != a.data(), "Input/Core array aliased.");
    equal(coreOrders, native, "Override modified Core-owned orders.");
    equal(other->GetRenderOrders(), native, "Override contaminated another model.");
    auto alteredInput = a;
    require(model->SetRenderOrderOverrideForEvaluation(alteredInput.data(), count), "Owned-copy test setup failed.");
    std::fill(alteredInput.begin(), alteredInput.end(), -1);
    equal(model->GetRenderOrders(), a, "Override did not copy caller memory.");
    require(model->SetRenderOrderOverrideForEvaluation(b.data(), count), "Permutation B rejected.");
    equal(model->GetRenderOrders(), b, "B not exposed.");
    equal(coreOrders, native, "B modified Core-owned orders.");
    const auto reject = [&](const Csm::csmInt32* input, int inputCount) {
        require(!model->SetRenderOrderOverrideForEvaluation(input, inputCount), "Invalid override accepted.");
        equal(model->GetRenderOrders(), b, "Invalid override left a partial/changed result.");
        equal(coreOrders, native, "Invalid override modified Core data.");
    };
    reject(nullptr, count); reject(nullptr, -1); reject(b.data(), 0);
    reject(b.data(), count - 1); reject(b.data(), count + 1); reject(b.data(), std::numeric_limits<int>::max());
    auto invalid = b;
    invalid.back() = invalid.front(); reject(invalid.data(), count);
    invalid = b; invalid.back() = -1; reject(invalid.data(), count);
    invalid = b; invalid.back() = count; reject(invalid.data(), count);
    // Input may be the previously returned owned buffer; validation must not
    // erase its data before the copy (scratch and output are separate vectors).
    require(model->SetRenderOrderOverrideForEvaluation(model->GetRenderOrders(), count), "Self-buffer assignment rejected.");
    equal(model->GetRenderOrders(), b, "Self-buffer assignment changed order.");
    const auto allocations = allocator.allocations;
    for (int trial = 0; trial < 64; ++trial) {
        const auto& selected = trial % 2 ? a : b;
        require(model->SetRenderOrderOverrideForEvaluation(selected.data(), count), "Reused-capacity override failed.");
        equal(model->GetRenderOrders(), selected, "Reused-capacity order mismatch.");
        require(model->SetRenderOrderOverrideForEvaluation(nullptr, 0), "Clear rejected.");
        require(model->GetRenderOrders() == Core::csmGetRenderOrders(core), "Clear did not return native Core array.");
        require(model->SetRenderOrderOverrideForEvaluation(selected.data(), count), "Pre-reset override failed.");
        require(moc->ResetModelForEvaluation(model.get()), "Core reset rejected.");
        require(model->GetModel() == core, "Core reset replaced model storage.");
        require(model->GetRenderOrders() == Core::csmGetRenderOrders(core), "Override survived Core reset.");
        model->Update();
        equal(model->GetRenderOrders(), native, "Core reset/update did not restore native order.");
        equal(other->GetRenderOrders(), native, "Reset contaminated another model.");
    }
    require(allocator.allocations == allocations, "Repeated override/clear/reset allocated Framework memory.");
    // Rejection after clearing cannot reactivate the old valid override.
    require(!model->SetRenderOrderOverrideForEvaluation(invalid.data(), count), "Invalid cleared override accepted.");
    require(model->GetRenderOrders() == Core::csmGetRenderOrders(core), "Rejected input reactivated stale override.");
    std::printf("PASS %s render_objects=%d offscreens=%d checks=%u retained vector capacity\n",
        modelPath.filename().u8string().c_str(), count, model->GetOffscreenCount(), checks);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::fprintf(stderr, "Usage: Live2DRenderOrderOverrideTest model3.json\n"); return 2; }
    Allocator allocator;
    Csm::CubismFramework::Option options{};
    options.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Off;
    if (!Csm::CubismFramework::StartUp(&allocator, &options)) return 3;
    Csm::CubismFramework::Initialize();
    int result = 0;
    try { run(fs::path(argv[1]), allocator); }
    catch (const std::exception& error) { std::fprintf(stderr, "ERROR: %s\n", error.what()); result = 1; }
    Csm::CubismFramework::Dispose(); Csm::CubismFramework::CleanUp();
    return result;
}
