#include "Renderer.h"
#include "MotionJson.h"

#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <malloc.h>

#include <CubismFramework.hpp>
#include <Id/CubismIdManager.hpp>
#include <CubismModelSettingJson.hpp>
#include <ICubismAllocator.hpp>
#include <Model/CubismUserModel.hpp>
#include <Model/CubismMoc.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionJson.hpp>
#include <Motion/CubismExpressionMotion.hpp>
#include <Motion/CubismMotionQueueEntry.hpp>
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <Utils/CubismJson.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace l2dae {
namespace {
namespace Csm = Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using D3DRenderer = Csm::Rendering::CubismRenderer_D3D11;

constexpr std::uint64_t kMaxFileBytes = 512ull * 1024 * 1024;
constexpr int kMaxDimension = 16384;
constexpr std::size_t kMaxModels = 4;
constexpr double kPhysicsStep = 1.0 / 60.0;
constexpr int kPhysicsWarmupSteps = 120;

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode path.");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const char* value) {
    if (!value || !*value) return {};
    const int bytes = static_cast<int>(std::strlen(value));
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, bytes, nullptr, 0);
    if (!size) throw std::runtime_error("A model file reference is not valid UTF-8.");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, bytes, result.data(), size);
    return result;
}

[[noreturn]] void failHr(const char* operation, HRESULT result) {
    std::ostringstream message;
    message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ").";
    throw std::runtime_error(message.str());
}
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) failHr(operation, result);
}

class ComScope {
    HRESULT result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
public:
    ComScope() {
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) failHr("COM initialization", result_);
    }
    ~ComScope() { if (SUCCEEDED(result_)) CoUninitialize(); }
};

std::vector<Csm::csmByte> readFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open: " + utf8(path.wstring()));
    const auto end = stream.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) > kMaxFileBytes)
        throw std::runtime_error("Empty or oversized file: " + utf8(path.wstring()));
    std::vector<Csm::csmByte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("Cannot read: " + utf8(path.wstring()));
    return bytes;
}

// Cubism's JSON reader accepts UTF-8, but does not accept JSON Unicode escapes.
// Normalize escaped Unicode without interpreting double-escaped backslashes.
std::vector<Csm::csmByte> readJson(const fs::path& path) {
    auto bytes = readFile(path);
    std::string input(bytes.begin(), bytes.end());
    if (input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xef &&
        static_cast<unsigned char>(input[1]) == 0xbb && static_cast<unsigned char>(input[2]) == 0xbf)
        input.erase(0, 3);
    std::string output;
    output.reserve(input.size());
    bool inString = false;
    auto hex4 = [&](std::size_t pos) {
        if (pos + 4 > input.size()) throw std::runtime_error("Incomplete JSON Unicode escape.");
        unsigned value = 0;
        for (std::size_t n = pos; n < pos + 4; ++n) {
            const char ch = input[n];
            const int digit = ch >= '0' && ch <= '9' ? ch - '0' :
                ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (digit < 0) throw std::runtime_error("Invalid JSON Unicode escape.");
            value = value * 16 + static_cast<unsigned>(digit);
        }
        return value;
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '"') inString = !inString;
        if (inString && ch == '\\' && i + 1 < input.size()) {
            if (input[i + 1] != 'u') {
                output += ch;
                output += input[++i];
                continue;
            }
            unsigned value = hex4(i + 2);
            i += 5;
            std::wstring decoded;
            if (value >= 0xd800 && value <= 0xdbff) {
                if (i + 6 >= input.size() || input[i + 1] != '\\' || input[i + 2] != 'u')
                    throw std::runtime_error("Unpaired JSON Unicode surrogate.");
                const unsigned low = hex4(i + 3);
                if (low < 0xdc00 || low > 0xdfff) throw std::runtime_error("Invalid JSON Unicode surrogate.");
                decoded += static_cast<wchar_t>(value);
                decoded += static_cast<wchar_t>(low);
                i += 6;
            } else {
                decoded += static_cast<wchar_t>(value);
            }
            if (value == '"') output += "\\\"";
            else if (value == '\\') output += "\\\\";
            else if (value == '\b') output += "\\b";
            else if (value == '\f') output += "\\f";
            else if (value == '\n') output += "\\n";
            else if (value == '\r') output += "\\r";
            else if (value == '\t') output += "\\t";
            else if (value < 32) throw std::runtime_error("Unsupported JSON control character.");
            else output += utf8(decoded);
        } else output += ch;
    }
    return {output.begin(), output.end()};
}

fs::path fullPath(const std::wstring& path) {
    if (path.empty()) throw std::runtime_error("Select a model3.json file first.");
    return fs::absolute(fs::path(path)).lexically_normal();
}

fs::path referencedFile(const fs::path& model, const char* reference) {
    const auto relative = fs::path(wide(reference));
    if (relative.empty()) throw std::runtime_error("A model file reference is empty.");
    return (model.parent_path() / relative).lexically_normal();
}

fs::path moduleDirectory() {
    static const int moduleAnchor = 0;
    HMODULE module = nullptr;
    check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleAnchor), &module) ? S_OK : HRESULT_FROM_WIN32(GetLastError()),
        "Locating the renderer module");
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) throw std::runtime_error("Cannot locate the renderer module.");
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

thread_local std::string frameworkError;
void logFramework(const char* message) {
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
    // Each render caller owns its diagnostics even when CPU evaluations overlap.
    frameworkError = message ? message : "Cubism Framework error.";
}
Csm::csmByte* loadShaderFile(const std::string path, Csm::csmSizeInt* size) {
    if (size) *size = 0;
    try {
        const auto bytes = readFile(moduleDirectory() / fs::path(wide(path.c_str())));
        auto* result = static_cast<Csm::csmByte*>(std::malloc(bytes.size()));
        if (!result) return nullptr;
        std::memcpy(result, bytes.data(), bytes.size());
        if (size) *size = static_cast<Csm::csmSizeInt>(bytes.size());
        return result;
    } catch (const std::exception& error) {
        frameworkError = error.what();
        return nullptr;
    }
}
void releaseShaderFile(Csm::csmByte* bytes) { std::free(bytes); }
void checkFramework() {
    if (!frameworkError.empty()) throw std::runtime_error("Live2D: " + frameworkError);
}

class Allocator final : public Csm::ICubismAllocator {
public:
    void* Allocate(Csm::csmSizeType size) override { return std::malloc(size); }
    void Deallocate(void* memory) override { std::free(memory); }
    void* AllocateAligned(Csm::csmSizeType size, Csm::csmUint32 alignment) override {
        return _aligned_malloc(size, alignment);
    }
    void DeallocateAligned(void* memory) override { _aligned_free(memory); }
};

class FrameworkScope {
    Allocator allocator_;
    Csm::CubismFramework::Option options_{};
public:
    FrameworkScope() {
        options_.LogFunction = logFramework;
        options_.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Error;
        options_.LoadFileFunction = loadShaderFile;
        options_.ReleaseBytesFunction = releaseShaderFile;
        if (!Csm::CubismFramework::StartUp(&allocator_, &options_))
            throw std::runtime_error("Cannot start the Cubism Framework.");
        Csm::CubismFramework::Initialize();
    }
    ~FrameworkScope() {
        // R5's CubismRenderer::StaticRelease is empty for D3D11. Its device registry
        // must be cleared explicitly while the Framework allocator is still alive.
        Csm::Rendering::CubismDeviceInfo_D3D11::ReleaseAllDeviceInfo();
        Csm::Rendering::CubismRenderer::StaticRelease();
        Csm::CubismFramework::Dispose();
        Csm::CubismFramework::CleanUp();
    }
};

std::unique_ptr<Csm::CubismModelSettingJson> modelSettings(const fs::path& path) {
    const auto bytes = readJson(path);
    auto settings = std::make_unique<Csm::CubismModelSettingJson>(bytes.data(), static_cast<int>(bytes.size()));
    if (!settings->GetJsonPointer()) throw std::runtime_error("Invalid model3.json: " + utf8(path.wstring()));
    if (!*settings->GetModelFileName()) throw std::runtime_error("model3.json does not reference a .moc3 model.");
    return settings;
}

struct FileStamp {
    fs::path path;
    fs::file_time_type modified;
    std::uintmax_t size;
    explicit FileStamp(fs::path value) : path(std::move(value)), modified(fs::last_write_time(path)), size(fs::file_size(path)) {}
    bool unchanged() const {
        std::error_code error;
        if (fs::last_write_time(path, error) != modified || error) return false;
        return fs::file_size(path, error) == size && !error;
    }
};

// The SDK's JSON numeric reader requires a comma/newline after numbers and does
// not accept exponent notation. Validate standard JSON first and produce its
// accepted spelling, so compact exp3 files and finite exponents work correctly.
class ExpressionJsonNormalizer {
    const std::string input_;
    std::size_t pos_ = 0;
    std::string output_;
    [[noreturn]] void invalid() const { throw std::runtime_error("Malformed expression JSON."); }
    void whitespace() {
        while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t' ||
            input_[pos_] == '\r' || input_[pos_] == '\n')) ++pos_;
    }
    bool take(char expected) {
        whitespace();
        if (pos_ == input_.size() || input_[pos_] != expected) return false;
        output_ += input_[pos_++]; output_ += '\n'; return true;
    }
    std::string string() {
        whitespace();
        const auto start = pos_;
        if (pos_ == input_.size() || input_[pos_++] != '"') invalid();
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c < 32) invalid();
            if (c == '"') {
                const auto value = input_.substr(start, pos_ - start);
                output_ += value; output_ += '\n'; return value;
            }
            if (c == '\\') {
                if (pos_ == input_.size() || std::string("\"\\/bfnrt").find(input_[pos_++]) == std::string::npos) invalid();
            }
        }
        invalid();
    }
    void number() {
        const auto start = pos_;
        if (input_[pos_] == '-') ++pos_;
        const auto digits = [&] { const auto first = pos_; while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_; return pos_ > first; };
        if (pos_ == input_.size()) invalid();
        if (input_[pos_] == '0') ++pos_;
        else if (input_[pos_] < '1' || input_[pos_] > '9' || !digits()) invalid();
        if (pos_ < input_.size() && input_[pos_] == '.') { ++pos_; if (!digits()) invalid(); }
        bool exponent = false;
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            exponent = true; ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (!digits()) invalid();
        }
        float parsed = 0;
        const auto result = std::from_chars(input_.data() + start, input_.data() + pos_, parsed);
        if (result.ec != std::errc{} || result.ptr != input_.data() + pos_ || !std::isfinite(parsed))
            throw std::runtime_error("Expression numbers must be finite and representable as 32-bit values.");
        if (exponent) {
            char decimal[256];
            const auto formatted = std::to_chars(decimal, decimal + sizeof decimal, parsed, std::chars_format::fixed);
            if (formatted.ec != std::errc{}) invalid();
            output_.append(decimal, formatted.ptr);
        } else output_.append(input_, start, pos_ - start);
        output_ += '\n';
    }
    void value(int depth) {
        whitespace();
        if (depth > 64 || pos_ == input_.size()) invalid();
        const char current = input_[pos_];
        if (current == '"') { string(); return; }
        if (current == '{') {
            take('{');
            if (take('}')) return;
            std::set<std::string> keys;
            do {
                if (!keys.insert(string()).second) throw std::runtime_error("Expression JSON contains duplicate object keys.");
                if (!take(':')) invalid();
                value(depth + 1);
                if (take('}')) return;
            } while (take(','));
            invalid();
        }
        if (current == '[') {
            take('[');
            if (take(']')) return;
            do { value(depth + 1); if (take(']')) return; } while (take(','));
            invalid();
        }
        for (const auto* literal : {"true", "false", "null"}) {
            const auto length = std::strlen(literal);
            if (input_.compare(pos_, length, literal) == 0) {
                output_ += literal; output_ += '\n'; pos_ += length; return;
            }
        }
        if (current == '-' || (current >= '0' && current <= '9')) { number(); return; }
        invalid();
    }
public:
    explicit ExpressionJsonNormalizer(const std::vector<Csm::csmByte>& input) : input_(input.begin(), input.end()) {}
    std::vector<Csm::csmByte> normalize() {
        value(0); whitespace();
        if (pos_ != input_.size()) invalid();
        return {output_.begin(), output_.end()};
    }
};

struct ExpressionData {
    fs::path path;
    std::vector<Csm::CubismExpressionMotion::ExpressionParameter> parameters;
};
struct ExpressionInfluence {
    int parameterIndex = 0;
    double additive = 0;
    double multiplyDeviation = 0;
    double overwriteSum = 0;
    double overwriteWeight = 0;
};
struct JsonDeleter { void operator()(Csm::Utils::CubismJson* json) const { Csm::Utils::CubismJson::Delete(json); } };
struct ExpressionDeleter { void operator()(Csm::CubismExpressionMotion* expression) const { Csm::ACubismMotion::Delete(expression); } };

std::shared_ptr<ExpressionData> readExpression(const fs::path& path) {
    if (fs::file_size(path) > 4 * 1024 * 1024) throw std::runtime_error("Expression file exceeds 4 MiB.");
    const auto input = readJson(path);
    if (input.size() > 4 * 1024 * 1024) throw std::runtime_error("Expression file exceeds 4 MiB.");
    const auto bytes = ExpressionJsonNormalizer(input).normalize();
    std::unique_ptr<Csm::Utils::CubismJson, JsonDeleter> json(Csm::Utils::CubismJson::Create(bytes.data(), static_cast<int>(bytes.size())));
    if (!json || !json->GetRoot().IsMap()) throw std::runtime_error("Expression must be a JSON object.");
    auto& root = json->GetRoot();
    if (!root["Type"].IsNull() && (!root["Type"].IsString() || std::strcmp(root["Type"].GetRawString(), "Live2D Expression") != 0))
        throw std::runtime_error("Unsupported expression Type; expected Live2D Expression.");
    auto& parameters = root["Parameters"];
    if (!parameters.IsArray() || parameters.GetSize() > 4096)
        throw std::runtime_error("Expression Parameters must be an array with at most 4096 entries.");
    for (const char* fade : {"FadeInTime", "FadeOutTime"}) {
        if (!root[fade].IsNull() && (!root[fade].IsFloat() || !std::isfinite(root[fade].ToFloat())))
            throw std::runtime_error("Expression fade times must be finite numbers.");
    }
    std::set<std::string> seen;
    for (int i = 0; i < parameters.GetSize(); ++i) {
        auto& parameter = parameters[i];
        if (!parameter.IsMap() || !parameter["Id"].IsString() || !*parameter["Id"].GetRawString() ||
            std::strlen(parameter["Id"].GetRawString()) > 1024)
            throw std::runtime_error("Expression parameter Id must be a nonempty string of at most 1024 UTF-8 bytes.");
        if (!seen.insert(parameter["Id"].GetRawString()).second)
            throw std::runtime_error("Expression contains duplicate parameter IDs.");
        if (!parameter["Value"].IsFloat() || !std::isfinite(parameter["Value"].ToFloat()))
            throw std::runtime_error("Expression parameter Value must be a finite number.");
        auto& blend = parameter["Blend"];
        if (blend.IsFloat()) {
            // Serialized Unity CubismExpressionData uses its public enum ordering:
            // CubismParameterBlendMode.Override=0, Additive=1, Multiply=2.
            // This deliberately differs from the native ExpressionBlendType enum.
            const float mode = blend.ToFloat();
            if (mode != 0 && mode != 1 && mode != 2)
                throw std::runtime_error("Numeric expression Blend must be Unity mode 0, 1, or 2.");
        } else if (!blend.IsNull() && (!blend.IsString() ||
            (std::strcmp(blend.GetRawString(), "Add") != 0 && std::strcmp(blend.GetRawString(), "Multiply") != 0 &&
             std::strcmp(blend.GetRawString(), "Overwrite") != 0)))
            throw std::runtime_error("Expression Blend must be Add, Multiply, or Overwrite.");
    }
    // Use the official parser's IDs and blend modes after stricter input validation.
    std::unique_ptr<Csm::CubismExpressionMotion, ExpressionDeleter> parsed(
        Csm::CubismExpressionMotion::Create(bytes.data(), static_cast<int>(bytes.size())));
    if (!parsed) throw std::runtime_error("Could not parse expression.");
    const auto officialParameters = parsed->GetExpressionParameters();
    auto result = std::make_shared<ExpressionData>();
    result->path = path;
    for (Csm::csmUint32 i = 0; i < officialParameters.GetSize(); ++i) {
        auto parameter = officialParameters[i];
        auto& blend = parameters[static_cast<int>(i)]["Blend"];
        if (blend.IsFloat()) {
            const auto mode = blend.ToInt();
            parameter.BlendType = mode == 0 ? Csm::CubismExpressionMotion::Overwrite :
                mode == 1 ? Csm::CubismExpressionMotion::Additive : Csm::CubismExpressionMotion::Multiply;
        }
        result->parameters.push_back(parameter);
    }
    checkFramework();
    return result;
}

ComPtr<ID3D11ShaderResourceView> loadTexture(ID3D11Device* device, const fs::path& path, std::uint64_t& byteCount) {
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf())), "Creating the PNG decoder");
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()), "Opening the model texture");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, frame.GetAddressOf()), "Reading the model texture");
    UINT width = 0, height = 0;
    check(frame->GetSize(&width, &height), "Reading texture dimensions");
    if (!width || !height || width > kMaxDimension || height > kMaxDimension ||
        static_cast<std::uint64_t>(width) * height * 4 > kMaxFileBytes)
        throw std::runtime_error("Model texture is too large.");
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(converter.GetAddressOf()), "Creating the texture converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom), "Converting the model texture");
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()),
        "Decoding the model texture");
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{rgba.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&description, &initial, texture.GetAddressOf()), "Uploading the model texture");
    ComPtr<ID3D11ShaderResourceView> view;
    check(device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()), "Binding the model texture");
    byteCount = static_cast<std::uint64_t>(width) * height * 4;
    return view;
}

struct MotionDeleter { void operator()(Csm::CubismMotion* motion) const { Csm::ACubismMotion::Delete(motion); } };

// All Framework parsing/ID interning and D3D operations use sdkMutex. Independent
// model evaluation uses cached IDs and may run without it. The allocator is CRT
// malloc/free and errors are thread-local; Framework lifetime is held separately.
std::mutex sdkMutex;
struct SharedTexture {
    FileStamp stamp;
    ComPtr<ID3D11ShaderResourceView> view;
    std::uint64_t bytes = 0;
    SharedTexture(ID3D11Device* device, const fs::path& path) : stamp(path) {
        view = loadTexture(device, path, bytes);
    }
};
struct SharedAssets {
    fs::path path;
    std::unique_ptr<Csm::CubismModelSettingJson> settings;
    std::shared_ptr<Csm::CubismMoc> moc;
    std::vector<FileStamp> dependencies;
    std::vector<std::shared_ptr<SharedTexture>> textures;
    std::vector<Csm::csmByte> physicsBytes, poseBytes;
    std::uint64_t mocBytes = 0;
    bool unchanged() const {
        return std::all_of(dependencies.begin(), dependencies.end(), [](const FileStamp& stamp) { return stamp.unchanged(); });
    }
};
struct ModelInstance {
    std::shared_ptr<Csm::CubismMoc> moc;
    Csm::CubismModel* model = nullptr;
    explicit ModelInstance(const std::shared_ptr<Csm::CubismMoc>& source) : moc(source), model(moc->CreateModel()) {
        if (!model) throw std::runtime_error("Cubism could not create a model instance.");
    }
    ~ModelInstance() { if (model) moc->DeleteModel(model); }
};

class Model final : public Csm::CubismUserModel {
    std::shared_ptr<SharedAssets> assets_;
    ModelInstance instance_;
    fs::path path_;
    Csm::CubismModelSettingJson* settings_;
    std::vector<float> initialPartOpacity_;
    std::vector<int> parameterIndices_;
    std::vector<float> initialParameters_;
    std::set<int> boundedVisibilityParameters_; // Explicit Part IDs / PartOpacity targets.
    std::vector<int> lipSyncIndices_;
    int breathIndex_ = -1;
    std::vector<int> eyeBlinkIndices_;
    struct PoseState {
        std::vector<float> parameters;
        std::vector<float> parts;
        float opacity = 1;
    };
    PoseState sourceScratchA_, sourceScratchB_;
    std::vector<float> commonPartsScratch_, eyeScratch_;
    std::vector<Csm::csmInt32> sourceOrdersA_, sourceOrdersB_;
    bool hasSourceRenderOrders_ = false;
    bool blendRenderOrders_ = false;
    bool useSourceBRenderOrder_ = false;
    struct CachedMotion {
        FileStamp stamp;
        std::shared_ptr<Csm::CubismMotion> motion;
    };
    std::list<CachedMotion> motions_;
    struct CachedExpression { FileStamp stamp; std::shared_ptr<ExpressionData> data; };
    std::list<CachedExpression> expressions_;

    void registerParameter(Csm::CubismIdHandle id) {
        const int index = _model->GetParameterIndex(id);
        if (std::find(parameterIndices_.begin(), parameterIndices_.end(), index) != parameterIndices_.end()) return;
        parameterIndices_.push_back(index);
        initialParameters_.push_back(_model->GetParameterValue(index));
    }

    void restoreParameters() {
        for (std::size_t i = 0; i < parameterIndices_.size(); ++i)
            _model->SetParameterValue(parameterIndices_[i], initialParameters_[i]);
        _model->SetModelOpacity(1.0f);
    }

    void restoreParts(const std::vector<float>& parts) {
        for (int i = 0; i < _model->GetPartCount(); ++i) _model->SetPartOpacity(i, parts[i]);
    }

    void capturePose(PoseState& state) const {
        state.parameters.resize(parameterIndices_.size());
        for (std::size_t i = 0; i < parameterIndices_.size(); ++i) state.parameters[i] = _model->GetParameterValue(parameterIndices_[i]);
        state.parts.resize(initialPartOpacity_.size());
        for (int i = 0; i < _model->GetPartCount(); ++i) state.parts[i] = _model->GetPartOpacity(i);
        state.opacity = _model->GetModelOpacity();
    }

    void sampleMotion(Csm::CubismMotion* motion, double time, bool loop) {
        restoreParameters();
        if (!motion) return;
        const double duration = motion->GetLoopDuration();
        time = std::max(0.0, time);
        time = loop ? std::fmod(time, duration) : std::min(time, duration);
        Csm::CubismMotionQueueEntry entry;
        entry.IsStarted(true);
        entry.SetStartTime(0);
        entry.SetFadeInStartTime(0);
        entry.SetEndTime(-1);
        // Absolute sampling deliberately disables transition fades; AE owns clip timing.
        motion->DoUpdateParameters(_model, static_cast<float>(time), 1.0f, &entry);
    }

    void sampleBlendedPose(Csm::CubismMotion* motionA, double secondsA,
        Csm::CubismMotion* motionB, double secondsB, float blend, bool loop) {
        // Preserve the preceding pose solver's part state as a common input to A/B.
        // Both sources reset all real and virtual pose-control parameters to the
        // same saved model baseline; A's writes never become B's starting state.
        if (blend == 0) { sampleMotion(motionA, secondsA, loop); return; }
        if (blend == 1) { sampleMotion(motionB, secondsB, loop); return; }
        commonPartsScratch_.resize(initialPartOpacity_.size());
        for (int i = 0; i < _model->GetPartCount(); ++i) commonPartsScratch_[i] = _model->GetPartOpacity(i);
        sampleMotion(motionA, secondsA, loop);
        capturePose(sourceScratchA_);
        restoreParts(commonPartsScratch_);
        sampleMotion(motionB, secondsB, loop);
        capturePose(sourceScratchB_);
        const auto& sourceA = sourceScratchA_;
        const auto& sourceB = sourceScratchB_;
        const auto interpolate = [](float a, float b, float weight) {
            return static_cast<float>(static_cast<double>(a) * (1.0 - weight) + static_cast<double>(b) * weight);
        };
        const float visibilityWeight = std::clamp(blend, 0.0f, 1.0f);
        for (std::size_t i = 0; i < parameterIndices_.size(); ++i) {
            const int index = parameterIndices_[i];
            // Pose and PartOpacity use explicit part controls, sometimes virtual
            // parameters. Extrapolate only real non-visibility parameters, never
            // a synthetic part switch or an opacity/visibility weight.
            const bool extrapolate = blend > 1 && index >= 0 && index < _model->GetParameterCount() &&
                boundedVisibilityParameters_.find(index) == boundedVisibilityParameters_.end();
            float value = interpolate(sourceA.parameters[i], sourceB.parameters[i],
                blend > 1 && !extrapolate ? visibilityWeight : blend);
            if (extrapolate) {
                // SetParameterValue wraps parameters marked Repeat. Clamp first
                // so elastic movement cannot wrap around a model's valid range.
                value = std::clamp(value, _model->GetParameterMinimumValue(index),
                    _model->GetParameterMaximumValue(index));
            }
            _model->SetParameterValue(index, value);
        }
        for (int i = 0; i < _model->GetPartCount(); ++i)
            _model->SetPartOpacity(i, interpolate(sourceA.parts[i], sourceB.parts[i], visibilityWeight));
        _model->SetModelOpacity(interpolate(sourceA.opacity, sourceB.opacity, visibilityWeight));
    }

    void applyExpressionOverlay(const std::vector<ExpressionInfluence>& overlay) {
        // The official SDK defines the operators as (overwrite + additive) *
        // multiplier. Timeline expressions aggregate these components together,
        // avoiding the manager's sequential, latest-expression-wins ordering.
        for (const auto& influence : overlay) {
            const double base = _model->GetParameterValue(influence.parameterIndex);
            const double overwritten = influence.overwriteWeight >= 1.0
                ? influence.overwriteSum / influence.overwriteWeight
                : base * (1.0 - influence.overwriteWeight) + influence.overwriteSum;
            const double value = (overwritten + influence.additive) * (1.0 + influence.multiplyDeviation);
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                throw std::runtime_error("Expression result is outside the supported parameter range.");
            _model->SetParameterValue(influence.parameterIndex, static_cast<float>(value));
        }
    }

    void applyLipSync(const RenderRequest& request) {
        if (!request.lipSyncEnabled) return;
        if (lipSyncIndices_.empty())
            throw std::runtime_error("This model has no supported mouth-opening parameter. Declare its parameter in the model3.json LipSync group.");
        for (const int index : lipSyncIndices_)
            _model->SetParameterValue(index, request.mouthOpen);
    }

public:
    Model(std::shared_ptr<SharedAssets> assets, ID3D11Device* device, int width, int height)
        : assets_(std::move(assets)), instance_(assets_->moc), path_(assets_->path), settings_(assets_->settings.get()) {
        try {
        // The base's _moc remains null: instance_ owns this mutable model and the
        // shared MOC owner outlives it. Create/DeleteModel run under sdkMutex.
        _model = instance_.model;
        if (!assets_->poseBytes.empty()) {
            LoadPose(assets_->poseBytes.data(), static_cast<int>(assets_->poseBytes.size()));
            if (!_pose) throw std::runtime_error("Invalid pose3.json file.");
            _pose->Reset(_model);
        }
        _model->SaveParameters();
        for (int i = 0; i < _model->GetParameterCount(); ++i) registerParameter(_model->GetParameterId(i));
        // Search actual MOC parameters instead of GetParameterIndex(), which creates
        // virtual parameters for missing IDs. Explicit LipSync declarations win;
        // fallback names are exact aliases and never include mouth shape/strength.
        const auto addLipSync = [&](Csm::CubismIdHandle id) {
            for (int i = 0; i < _model->GetParameterCount(); ++i) {
                if (_model->GetParameterId(i) == id &&
                    std::find(lipSyncIndices_.begin(), lipSyncIndices_.end(), i) == lipSyncIndices_.end())
                    lipSyncIndices_.push_back(i);
            }
        };
        for (int i = 0; i < settings_->GetLipSyncParameterCount(); ++i)
            addLipSync(settings_->GetLipSyncParameterId(i));
        if (lipSyncIndices_.empty()) addLipSync(Csm::CubismFramework::GetIdManager()->GetId("ParamMouthOpenY"));
        if (lipSyncIndices_.empty()) addLipSync(Csm::CubismFramework::GetIdManager()->GetId("PARAM_MOUTH_OPEN_Y"));
        const auto actualIndex = [&](Csm::CubismIdHandle id) {
            for (int i = 0; i < _model->GetParameterCount(); ++i)
                if (_model->GetParameterId(i) == id) return i;
            return -1;
        };
        const auto namedIndex = [&](const char* name) {
            return actualIndex(Csm::CubismFramework::GetIdManager()->GetId(name));
        };
        breathIndex_ = namedIndex("ParamBreath");
        if (breathIndex_ < 0) breathIndex_ = namedIndex("PARAM_BREATH");
        const auto addEye = [&](int index) {
            if (index >= 0 && std::find(eyeBlinkIndices_.begin(), eyeBlinkIndices_.end(), index) == eyeBlinkIndices_.end())
                eyeBlinkIndices_.push_back(index);
        };
        for (int i = 0; i < settings_->GetEyeBlinkParameterCount(); ++i)
            addEye(actualIndex(settings_->GetEyeBlinkParameterId(i)));
        if (eyeBlinkIndices_.empty()) {
            const int left = namedIndex("ParamEyeLOpen"), right = namedIndex("ParamEyeROpen");
            addEye(left >= 0 ? left : namedIndex("PARAM_EYE_L_OPEN"));
            addEye(right >= 0 ? right : namedIndex("PARAM_EYE_R_OPEN"));
        }
        for (int i = 0; i < _model->GetPartCount(); ++i) initialPartOpacity_.push_back(_model->GetPartOpacity(i));
        // Cubism Pose uses parameters named after parts, even when the MOC has no
        // actual parameter with that ID. Include these virtual controls in blending.
        for (int i = 0; i < _model->GetPartCount(); ++i) {
            const auto id = _model->GetPartId(i);
            registerParameter(id);
            boundedVisibilityParameters_.insert(_model->GetParameterIndex(id));
        }
        D3DRenderer::SetConstantSettings(1, device);
        CreateRenderer(static_cast<Csm::csmUint32>(width), static_cast<Csm::csmUint32>(height), 1);
        auto* renderer = GetRenderer<D3DRenderer>();
        if (!renderer) throw std::runtime_error("Cannot create the Cubism D3D11 renderer.");
        renderer->IsPremultipliedAlpha(false); // WIC uploaded straight alpha; Cubism outputs premultiplied alpha.
        renderer->UseHighPrecisionMask(true);
        const int count = settings_->GetTextureCount();
        if (count <= 0) throw std::runtime_error("model3.json has no textures.");
        for (int i = 0; i < count; ++i) {
            renderer->BindTexture(static_cast<Csm::csmUint32>(i), assets_->textures[i]->view.Get());
        }
        checkFramework();
        } catch (...) {
            // The renderer must be destroyed before instance_ unwinds on failure.
            DeleteRenderer();
            _model = nullptr;
            throw;
        }
    }
    ~Model() override { DeleteRenderer(); _model = nullptr; }

    const fs::path& path() const { return path_; }
    const SharedAssets& assets() const { return *assets_; }
    float opacity() const { return std::clamp(_model->GetModelOpacity(), 0.0f, 1.0f); }
    bool unchanged() const {
        return assets_->unchanged();
    }

    std::shared_ptr<Csm::CubismMotion> motion(const std::wstring& requested) {
        if (requested.empty()) return nullptr;
        const auto path = fullPath(requested);
        for (auto it = motions_.begin(); it != motions_.end(); ++it) {
            if (it->stamp.path == path) {
                if (!it->stamp.unchanged()) { motions_.erase(it); break; }
                motions_.splice(motions_.begin(), motions_, it);
                return motions_.front().motion;
            }
        }
        const auto bytes = normalizeMotionJson(readJson(path)).bytes;
        std::shared_ptr<Csm::CubismMotion> loaded(
            Csm::CubismMotion::Create(bytes.data(), static_cast<int>(bytes.size()), nullptr, nullptr, true), MotionDeleter{});
        if (!loaded || !std::isfinite(loaded->GetLoopDuration()) || loaded->GetLoopDuration() <= 0)
            throw std::runtime_error("Invalid or zero-duration motion3.json.");
        loaded->SetLoop(false);
        loaded->SetFadeInTime(0);
        loaded->SetFadeOutTime(0);
        Csm::csmVector<Csm::CubismIdHandle> eyes, lips;
        for (int i = 0; i < settings_->GetEyeBlinkParameterCount(); ++i) eyes.PushBack(settings_->GetEyeBlinkParameterId(i));
        for (int i = 0; i < settings_->GetLipSyncParameterCount(); ++i) lips.PushBack(settings_->GetLipSyncParameterId(i));
        loaded->SetEffectIds(eyes, lips);
        Csm::CubismMotionJson json(bytes.data(), static_cast<int>(bytes.size()));
        for (int i = 0; i < json.GetMotionCurveCount(); ++i) {
            const char* target = json.GetMotionCurveTarget(i);
            if (std::strcmp(target, "Parameter") == 0 || std::strcmp(target, "PartOpacity") == 0) {
                const auto id = json.GetMotionCurveId(i);
                registerParameter(id);
                if (std::strcmp(target, "PartOpacity") == 0)
                    boundedVisibilityParameters_.insert(_model->GetParameterIndex(id));
                loaded->SetParameterFadeInTime(id, 0);
                loaded->SetParameterFadeOutTime(id, 0);
            }
        }
        for (int i = 0; i < _model->GetParameterCount(); ++i) {
            const auto id = _model->GetParameterId(i);
            loaded->SetParameterFadeInTime(id, 0);
            loaded->SetParameterFadeOutTime(id, 0);
        }
        // DoUpdateParameters lazily interns its EyeBlink/LipSync/Opacity IDs and
        // mutates motion state. Prime this worker-local object under sdkMutex,
        // before any CPU evaluation can overlap another worker's preparation.
        sampleMotion(loaded.get(), 0, true);
        restoreParameters();
        restoreParts(initialPartOpacity_);
        motions_.push_front({FileStamp(path), std::move(loaded)});
        while (motions_.size() > 16) motions_.pop_back();
        return motions_.front().motion;
    }

    std::shared_ptr<ExpressionData> expression(const std::wstring& requested) {
        if (requested.empty()) return nullptr;
        const auto path = fullPath(requested);
        for (auto it = expressions_.begin(); it != expressions_.end(); ++it) {
            if (it->stamp.path == path) {
                if (!it->stamp.unchanged()) { expressions_.erase(it); break; }
                expressions_.splice(expressions_.begin(), expressions_, it);
                return expressions_.front().data;
            }
        }
        auto data = readExpression(path);
        for (const auto& parameter : data->parameters) registerParameter(parameter.ParameterId);
        expressions_.push_front({FileStamp(path), std::move(data)});
        while (expressions_.size() > 16) expressions_.pop_back();
        return expressions_.front().data;
    }

    std::vector<ExpressionInfluence> expressionOverlay(const std::shared_ptr<ExpressionData>& expressionA,
        float weightA, const std::shared_ptr<ExpressionData>& expressionB, float weightB) {
        std::map<int, ExpressionInfluence> combined;
        const auto append = [&](const std::shared_ptr<ExpressionData>& expression, double weight) {
            if (!expression || weight == 0) return;
            for (const auto& parameter : expression->parameters) {
                const int index = _model->GetParameterIndex(parameter.ParameterId);
                auto& influence = combined[index];
                influence.parameterIndex = index;
                switch (parameter.BlendType) {
                case Csm::CubismExpressionMotion::Additive:
                    influence.additive += weight * parameter.Value; break;
                case Csm::CubismExpressionMotion::Multiply:
                    influence.multiplyDeviation += weight * (static_cast<double>(parameter.Value) - 1.0); break;
                case Csm::CubismExpressionMotion::Overwrite:
                    // Match CubismModel::SetParameterValue(target, weight): normalize
                    // each absolute target before weighting, not just the final mix.
                    {
                        float target = parameter.Value;
                        if (index < _model->GetParameterCount())
                            target = _model->IsRepeat(index) ? _model->GetParameterRepeatValue(index, target)
                                : _model->GetParameterClampValue(index, target);
                        influence.overwriteSum += weight * target;
                    }
                    influence.overwriteWeight += weight; break;
                }
            }
        };
        std::error_code error;
        const bool sameFile = expressionA && expressionB && (expressionA == expressionB ||
            fs::equivalent(expressionA->path, expressionB->path, error));
        if (sameFile) append(expressionA, std::min(1.0, static_cast<double>(weightA) + weightB));
        else { append(expressionA, weightA); append(expressionB, weightB); }
        std::vector<ExpressionInfluence> result;
        result.reserve(combined.size());
        for (const auto& pair : combined) result.push_back(pair.second);
        return result;
    }

    void evaluate(Csm::CubismMotion* motionA, Csm::CubismMotion* motionB,
        const std::vector<ExpressionInfluence>& expressions, const RenderRequest& request,
        bool collectRenderOrders = true) {
        if (collectRenderOrders) {
            hasSourceRenderOrders_ = false;
            blendRenderOrders_ = false;
            useSourceBRenderOrder_ = false;
            if (request.blend > 0 && request.blend != 1 &&
                !(motionA == motionB && request.seconds == request.secondsB)) {
                // Draw order is discrete even when the driving parameter is
                // continuous. Evaluate the two complete source poses (including
                // their physics/expressions) only to obtain valid order tables.
                // The final mixed geometry is evaluated once below and is shared
                // by both drawing passes: no double silhouette or pose dissolve.
                const int count = _model->GetDrawableCount() + _model->GetOffscreenCount();
                const auto captureOrders = [&](std::vector<Csm::csmInt32>& orders) {
                    if (count <= 0) { orders.clear(); return; }
                    const auto* values = _model->GetRenderOrders();
                    orders.assign(values, values + count);
                };
                const auto reset = [&] {
                    std::lock_guard<std::mutex> lock(sdkMutex);
                    resetCoreForEvaluation();
                };
                auto source = request;
                if (request.blend < 1) {
                    source.blend = 0;
                    evaluate(motionA, motionB, expressions, source, false);
                    captureOrders(sourceOrdersA_);
                    reset();
                }
                source.blend = 1;
                evaluate(motionA, motionB, expressions, source, false);
                captureOrders(sourceOrdersB_);
                reset();
                hasSourceRenderOrders_ = true;
                // Above B, only geometry overshoots. Keep B's complete valid
                // visibility order in one pass; no negative image weights.
                useSourceBRenderOrder_ = request.blend > 1;
                blendRenderOrders_ = !useSourceBRenderOrder_ && sourceOrdersA_ != sourceOrdersB_;
            }
        }
        const double endA = std::max(0.0, request.seconds);
        const double endB = std::max(0.0, request.secondsB);
        const bool breathAvailable = request.breathingEnabled && breathIndex_ >= 0;
        const bool blinkAvailable = request.autoBlinkEnabled && !eyeBlinkIndices_.empty();
        if (breathAvailable && (!std::isfinite(request.breathingAmount) || request.breathingAmount < 0 || request.breathingAmount > 1))
            throw std::runtime_error("Invalid breathing amount.");
        if (blinkAvailable && (!std::isfinite(request.blinkStrength) || request.blinkStrength < 0 || request.blinkStrength > 1))
            throw std::runtime_error("Invalid blink strength.");
        const bool breathe = breathAvailable && request.breathingAmount > 0;
        const bool blink = blinkAvailable && request.blinkStrength > 0;
        if (breathe && (!std::isfinite(request.breathingPeriod) || request.breathingPeriod < 0.1 || request.breathingPeriod > 60))
            throw std::runtime_error("Invalid breathing period.");
        if (blink && (!std::isfinite(request.blinkInterval) || request.blinkInterval < 0.2 || request.blinkInterval > 60 ||
            !std::isfinite(request.blinkDuration) || request.blinkDuration < 0.02 || request.blinkDuration > 2))
            throw std::runtime_error("Invalid blink interval or duration.");
        const bool ambient = breathe || blink;
        // Unavailable controls are a true no-op, including the physics history.
        if (ambient && !std::isfinite(request.ambientSeconds))
            throw std::runtime_error("Invalid breathing or auto-blink layer time.");
        const double warmup = std::min(kPhysicsWarmupSteps * kPhysicsStep,
            std::max({request.blend != 1 ? endA : 0.0, request.blend > 0 ? endB : 0.0,
                ambient ? std::max(0.0, request.ambientSeconds) : 0.0}));
        eyeScratch_.resize(blink ? eyeBlinkIndices_.size() : 0);
        auto& finalEyeValues = eyeScratch_;
        float finalBreathValue = 0;
        const auto applyAmbient = [&](double time) {
            if (!ambient) return;
            // Absolute phase also supports negative layer time and reverse seeking.
            if (breathe) {
                double phase = std::fmod(time, request.breathingPeriod);
                if (phase < 0) phase += request.breathingPeriod;
                const double unit = 0.5 - 0.5 * std::cos(phase * (6.28318530717958647692 / request.breathingPeriod));
                const double minimum = _model->GetParameterMinimumValue(breathIndex_);
                const double maximum = _model->GetParameterMaximumValue(breathIndex_);
                finalBreathValue = static_cast<float>(minimum + (maximum - minimum) * (unit * request.breathingAmount));
                _model->SetParameterValue(breathIndex_, finalBreathValue);
            }
            if (blink) {
                double factor = 1;
                if (request.blinkInterval == 4.0 && request.blinkDuration == 0.3) {
                    // Keep existing projects bit-identical at the original defaults.
                    double phase = std::fmod(time, 4.0);
                    if (phase < 0) phase += 4.0;
                    if (phase >= 3.0 && phase < 3.1) factor = (3.1 - phase) / 0.1;
                    else if (phase >= 3.1 && phase < 3.15) factor = 0;
                    else if (phase >= 3.15 && phase < 3.3) factor = (phase - 3.15) / 0.15;
                } else {
                    double phase = std::fmod(time - request.blinkInterval * 0.75, request.blinkInterval);
                    if (phase < 0) phase += request.blinkInterval;
                    const double duration = std::min(request.blinkDuration, request.blinkInterval);
                    if (phase < duration / 3) factor = 1 - phase / (duration / 3);
                    else if (phase < duration / 2) factor = 0;
                    else if (phase < duration) factor = (phase - duration / 2) / (duration / 2);
                }
                factor = std::clamp(factor, 0.0, 1.0);
                if (request.blinkStrength != 1) factor = 1 - request.blinkStrength * (1 - factor);
                for (std::size_t i = 0; i < eyeBlinkIndices_.size(); ++i) {
                    finalEyeValues[i] = static_cast<float>(_model->GetParameterValue(eyeBlinkIndices_[i]) * factor);
                    _model->SetParameterValue(eyeBlinkIndices_[i], finalEyeValues[i]);
                }
            }
        };
        const auto sample = [&](double elapsed) {
            const double age = warmup - elapsed;
            sampleBlendedPose(motionA, std::max(0.0, endA - age), motionB,
                std::max(0.0, endB - age), request.blend, request.loop);
            applyExpressionOverlay(expressions);
            applyAmbient(ambient ? request.ambientSeconds - age : 0.0);
            applyLipSync(request);
        };
        restoreParameters();
        restoreParts(initialPartOpacity_);
        if (_pose) _pose->Reset(_model);
        // The pinned R5 build extension restores exactly fresh Create() state,
        // including its fractional clock and typed particles, without reparsing.
        if (_physics) _physics->ResetForEvaluation();
        // The bounded pre-roll and independent clocks retain the serial behavior.
        sample(0);
        if (_physics) _physics->Stabilization(_model);
        if (_pose) _pose->UpdateParameters(_model, 0);
        if (_physics || _pose) {
            const int steps = std::min(kPhysicsWarmupSteps, static_cast<int>(std::ceil(warmup / kPhysicsStep)));
            double previous = 0;
            for (int i = 1; i <= steps; ++i) {
                const double time = i == steps ? warmup : std::min(warmup, i * kPhysicsStep);
                sample(time);
                const float delta = static_cast<float>(time - previous);
                if (_physics) _physics->Evaluate(_model, delta);
                if (_pose) _pose->UpdateParameters(_model, delta);
                previous = time;
            }
        } else sample(warmup);
        // Reapply the final targets, never multiply the already-blinked values.
        // Solvers cannot reopen an expression-closed eye or replace dedicated breath.
        if (breathe) _model->SetParameterValue(breathIndex_, finalBreathValue);
        if (blink) for (std::size_t i = 0; i < eyeBlinkIndices_.size(); ++i)
            _model->SetParameterValue(eyeBlinkIndices_[i], finalEyeValues[i]);
        // A physics/pose output must not reopen the mouth during silence. Applying
        // once more here gives the audio envelope final ownership of opening only.
        applyLipSync(request);
        _model->Update();
    }

    void preparePhysics() {
        // Parse once per mutable model under sdkMutex. Subsequent frames reset
        // only this worker's typed state in the parallel CPU region.
        if (!_physics && !assets_->physicsBytes.empty()) {
            LoadPhysics(assets_->physicsBytes.data(), static_cast<int>(assets_->physicsBytes.size()));
            if (!_physics) throw std::runtime_error("Invalid physics3.json file.");
        }
    }

    void resetCoreForEvaluation() {
        // Core retains incremental deformation state beyond public parameters.
        // Reinitialize only this worker's existing Core buffer through the SDK
        // extension; preserve the Framework model, motion/texture caches and GPU.
        if (!instance_.moc->ResetModelForEvaluation(_model))
            throw std::runtime_error("Cubism could not reset model geometry for independent frame evaluation.");
    }

    bool blendsRenderOrders() const { return blendRenderOrders_; }

    void selectRenderOrderPass(int pass) {
        if (!hasSourceRenderOrders_ || pass < 0) {
            if (!_model->SetRenderOrderOverrideForEvaluation(nullptr, 0))
                throw std::runtime_error("Could not clear the model drawing order.");
            return;
        }
        const auto& orders = pass == 0 && !useSourceBRenderOrder_ ? sourceOrdersA_ : sourceOrdersB_;
        if (!_model->SetRenderOrderOverrideForEvaluation(orders.data(), static_cast<int>(orders.size())))
            throw std::runtime_error("The transition produced an invalid model drawing order.");
    }

    void draw(const RenderRequest& request, ID3D11DeviceContext* context) {
        Core::csmVector2 canvas{}, origin{};
        float pixelsPerUnit = 0;
        Core::csmReadCanvasInfo(_model->GetModel(), &canvas, &origin, &pixelsPerUnit);
        if (canvas.X <= 0 || canvas.Y <= 0 || pixelsPerUnit <= 0)
            throw std::runtime_error("The model has invalid canvas dimensions.");
        const float widthUnits = canvas.X / pixelsPerUnit;
        const float heightUnits = canvas.Y / pixelsPerUnit;
        const float physicalWidth = request.width * request.pixelAspect;
        const float fit = std::min(physicalWidth / widthUnits, request.height / heightUnits) * request.scale;
        const float sx = 2.0f * fit / physicalWidth;
        const float sy = 2.0f * fit / request.height;
        const float centerX = (canvas.X * 0.5f - origin.X) / pixelsPerUnit;
        const float centerY = (canvas.Y * 0.5f - origin.Y) / pixelsPerUnit;
        Csm::CubismMatrix44 matrix;
        matrix.Scale(sx, sy);
        matrix.Translate(2.0f * request.offsetX / request.width - centerX * sx,
            -2.0f * request.offsetY / request.height - centerY * sy);
        auto* renderer = GetRenderer<D3DRenderer>();
        renderer->SetRenderTargetSize(request.width, request.height);
        renderer->SetMvpMatrix(&matrix);
        // Model opacity is applied once after compositing, so overlapping meshes fade together.
        renderer->SetModelColor(1, 1, 1, 1);
        renderer->StartFrame(context);
        renderer->DrawModel();
        renderer->EndFrame();
        checkFramework();
    }
};

class Engine {
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> target_, staging_;
    ComPtr<ID3D11RenderTargetView> targetView_;
    int width_ = 0, height_ = 0;
    struct Slot {
        std::unique_ptr<Model> model;
        bool busy = false;
        std::uint64_t lastUse = 0;
    };
    std::array<Slot, kMaxModels> slots_;
    const std::uint32_t workerLimit_ = std::min<std::uint32_t>(static_cast<std::uint32_t>(kMaxModels),
        std::max(1u, std::thread::hardware_concurrency()));
    std::mutex poolMutex_;
    std::condition_variable available_;
    std::uint64_t useCounter_ = 0;
    std::map<fs::path, std::weak_ptr<SharedAssets>> assets_;
    std::map<fs::path, std::weak_ptr<SharedTexture>> textures_;
    std::atomic<std::uint32_t> activeCpu_{0}, peakCpu_{0};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<bool> deviceFailed_{false};

    class Lease {
        Engine& engine_;
    public:
        Slot& slot;
        Lease(Engine& engine, Slot& value) : engine_(engine), slot(value) {}
        Lease(const Lease&) = delete;
        ~Lease() {
            { std::lock_guard<std::mutex> lock(engine_.poolMutex_); slot.busy = false; }
            engine_.available_.notify_one();
        }
    };

    Slot& acquire(const fs::path& path) {
        std::unique_lock<std::mutex> lock(poolMutex_);
        for (;;) {
            Slot* selected = nullptr;
            for (std::uint32_t i = 0; i < workerLimit_; ++i) {
                auto& candidate = slots_[i];
                // A busy slot's model is exclusively owned by its caller. Only
                // inspect cached models after the previous lease was returned.
                if (candidate.busy) continue;
                if (candidate.model && candidate.model->path() == path) { selected = &candidate; break; }
                if (!selected || (!candidate.model && selected->model) ||
                    (static_cast<bool>(candidate.model) == static_cast<bool>(selected->model) && candidate.lastUse < selected->lastUse))
                    selected = &candidate;
            }
            if (selected) {
                selected->busy = true;
                selected->lastUse = ++useCounter_;
                return *selected;
            }
            available_.wait(lock);
        }
    }

    std::shared_ptr<SharedAssets> loadAssets(const fs::path& path) {
        // Weak caches retain only assets owned by the four model slots. Pruning
        // also bounds the path index while an editor visits many different files.
        for (auto it = assets_.begin(); it != assets_.end(); )
            if (it->second.expired()) it = assets_.erase(it); else ++it;
        for (auto it = textures_.begin(); it != textures_.end(); )
            if (it->second.expired()) it = textures_.erase(it); else ++it;
        const auto found = assets_.find(path);
        if (found != assets_.end()) {
            auto shared = found->second.lock();
            if (shared && shared->unchanged()) return shared;
            assets_.erase(found);
        }
        auto result = std::make_shared<SharedAssets>();
        result->path = path;
        result->settings = modelSettings(path);
        result->dependencies.emplace_back(path);
        const auto mocPath = referencedFile(path, result->settings->GetModelFileName());
        const auto mocBytes = readFile(mocPath);
        result->mocBytes = mocBytes.size();
        result->moc = std::shared_ptr<Csm::CubismMoc>(
            Csm::CubismMoc::Create(mocBytes.data(), static_cast<int>(mocBytes.size()), true), Csm::CubismMoc::Delete);
        if (!result->moc) throw std::runtime_error("Cubism could not load this MOC3 file; check its export version.");
        result->dependencies.emplace_back(mocPath);
        if (*result->settings->GetPhysicsFileName()) {
            const auto file = referencedFile(path, result->settings->GetPhysicsFileName());
            result->physicsBytes = readJson(file);
            result->dependencies.emplace_back(file);
        }
        if (*result->settings->GetPoseFileName()) {
            const auto file = referencedFile(path, result->settings->GetPoseFileName());
            result->poseBytes = readJson(file);
            result->dependencies.emplace_back(file);
        }
        const int textureCount = result->settings->GetTextureCount();
        if (textureCount <= 0) throw std::runtime_error("model3.json has no textures.");
        for (int i = 0; i < textureCount; ++i) {
            const auto file = referencedFile(path, result->settings->GetTextureFileName(i));
            auto& cached = textures_[file];
            auto texture = cached.lock();
            if (!texture || !texture->stamp.unchanged()) {
                texture = std::make_shared<SharedTexture>(device_.Get(), file);
                cached = texture;
            }
            result->textures.push_back(std::move(texture));
            result->dependencies.emplace_back(file);
        }
        assets_[path] = result;
        return result;
    }

    class CpuEvaluation {
        Engine& engine_;
    public:
        explicit CpuEvaluation(Engine& engine) : engine_(engine) {
            const auto now = engine_.activeCpu_.fetch_add(1) + 1;
            auto peak = engine_.peakCpu_.load();
            while (peak < now && !engine_.peakCpu_.compare_exchange_weak(peak, now)) {}
        }
        ~CpuEvaluation() { engine_.activeCpu_.fetch_sub(1); }
    };
public:
    Engine() {
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL obtained{};
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
            device_.GetAddressOf(), &obtained, context_.GetAddressOf());
        if (FAILED(result)) {
            device_.Reset(); context_.Reset();
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
                device_.GetAddressOf(), &obtained, context_.GetAddressOf());
        }
        check(result, "Creating a Direct3D 11 device");
        // Catch missing shader deployment before Framework enters its drawing code.
        readFile(moduleDirectory() / L"FrameworkShaders/CubismEffect.fx");
        readFile(moduleDirectory() / L"FrameworkShaders/CubismBlendMode.fx");
    }
    ~Engine() {
        // Lifecycle's exclusive lock has already drained every outstanding lease.
        for (auto& slot : slots_) slot.model.reset();
        assets_.clear(); textures_.clear();
        if (context_) { context_->ClearState(); context_->Flush(); }
        Csm::Rendering::CubismDeviceInfo_D3D11::ReleaseAllDeviceInfo();
    }

    void ensureTarget(int width, int height) {
        if (width == width_ && height == height_) return;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        targetView_.Reset(); target_.Reset(); staging_.Reset();
        width_ = height_ = 0;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device_->CreateTexture2D(&description, nullptr, target_.GetAddressOf()), "Creating the offscreen target");
        check(device_->CreateRenderTargetView(target_.Get(), nullptr, targetView_.GetAddressOf()), "Creating the offscreen view");
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device_->CreateTexture2D(&description, nullptr, staging_.GetAddressOf()), "Creating the pixel readback buffer");
        width_ = width; height_ = height;
    }

    RenderResult render(const RenderRequest& request) {
        const auto path = fullPath(request.modelPath);
        Lease lease(*this, acquire(path));
        try {
        std::shared_ptr<Csm::CubismMotion> motionA, motionB;
        std::vector<ExpressionInfluence> overlay;
        {
            std::lock_guard<std::mutex> lock(sdkMutex);
            if (deviceFailed_) throw std::runtime_error("Direct3D device recovery is pending.");
            if (lease.slot.model && (lease.slot.model->path() != path || !lease.slot.model->unchanged()))
                lease.slot.model.reset();
            if (!lease.slot.model)
                lease.slot.model = std::make_unique<Model>(loadAssets(path), device_.Get(), request.width, request.height);
            auto& current = *lease.slot.model;
            // Sources remain pinned while only this lease can mutate the model.
            motionA = request.blend != 1 ? current.motion(request.motionPath) : nullptr;
            motionB = request.blend > 0 ? current.motion(request.motionPathB) : nullptr;
            const auto expressionA = request.expressionWeightA > 0 ? current.expression(request.expressionPathA) : nullptr;
            const auto expressionB = request.expressionWeightB > 0 ? current.expression(request.expressionPathB) : nullptr;
            overlay = current.expressionOverlay(expressionA, request.expressionWeightA, expressionB, request.expressionWeightB);
            current.preparePhysics();
            current.resetCoreForEvaluation();
        }
        auto& current = *lease.slot.model;
        {
            CpuEvaluation evaluating(*this);
            current.evaluate(motionA.get(), motionB.get(), overlay, request);
            checkFramework();
        }
        RenderResult output{request.width, request.height,
            std::vector<std::uint8_t>(static_cast<std::size_t>(request.width) * request.height * 4)};
        {
        // R5's renderer/device/shader state is mutable. CPU work in other slots
        // continues while this frame submits and reads back through one context.
        std::lock_guard<std::mutex> lock(sdkMutex);
        ensureTarget(request.width, request.height);
        const int passes = current.blendsRenderOrders() ? 2 : 1;
        for (int pass = 0; pass < passes; ++pass) {
        // Equal endpoint tables use their shared order in a single pass; an
        // intermediate pose must not invent a third, transient occlusion order.
        current.selectRenderOrderPass(pass);
        context_->ClearState();
        ID3D11RenderTargetView* target = targetView_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(request.width), static_cast<float>(request.height), 0, 1};
        context_->RSSetViewports(1, &viewport);
        const float clear[4]{};
        context_->ClearRenderTargetView(target, clear);
        current.draw(request, context_.Get());
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->CopyResource(staging_.Get(), target_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Reading the rendered pixels");
        const std::size_t pitch = static_cast<std::size_t>(request.width) * 4;
        for (int y = 0; y < request.height; ++y) {
            auto* destination = output.rgba.data() + static_cast<std::size_t>(y) * pitch;
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
            if (pass == 0) std::memcpy(destination, source, pitch);
            else {
                // Both images have identical vertices, masks and material state.
                // Only their occlusion order differs. Blend premultiplied channels
                // directly from staging, without another frame-sized allocation.
                const double weight = std::clamp(static_cast<double>(request.blend), 0.0, 1.0);
                for (std::size_t c = 0; c < pitch; ++c)
                    destination[c] = static_cast<std::uint8_t>(std::lround(
                        destination[c] * (1.0 - weight) + source[c] * weight));
            }
        }
        context_->Unmap(staging_.Get(), 0);
        check(device_->GetDeviceRemovedReason(), "Direct3D rendering");
        }
        current.selectRenderOrderPass(-1);
        }
        const float opacity = current.opacity();
        if (opacity < 1.0f) {
            for (auto& channel : output.rgba)
                channel = static_cast<std::uint8_t>(std::lround(channel * opacity));
        }
        frames_.fetch_add(1);
        return output;
        } catch (...) {
            std::lock_guard<std::mutex> lock(sdkMutex);
            // Other leases retain their models and Framework. Only device removal
            // retires the whole generation, after the lifecycle lock drains it.
            if (FAILED(device_->GetDeviceRemovedReason())) deviceFailed_ = true;
            lease.slot.model.reset();
            throw;
        }
    }

    bool deviceFailed() const { return deviceFailed_.load(); }
    RendererStats stats() const {
        // Caller holds sdkMutex, so slot model ownership and weak caches are stable.
        RendererStats result;
        result.workerLimit = workerLimit_;
        std::set<const SharedAssets*> liveAssets;
        std::set<const SharedTexture*> liveTextures;
        for (const auto& slot : slots_) if (slot.model) {
            ++result.modelInstances;
            const auto& asset = slot.model->assets();
            if (liveAssets.insert(&asset).second) result.sharedModelBytes += asset.mocBytes;
            for (const auto& texture : asset.textures)
                if (liveTextures.insert(texture.get()).second) result.sharedTextureBytes += texture->bytes;
        }
        result.workerCount = result.modelInstances;
        result.activeCpuEvaluations = activeCpu_.load();
        result.peakCpuConcurrency = peakCpu_.load();
        result.framesRendered = frames_.load();
        return result;
    }
};

std::shared_mutex rendererLifecycle;
// A single Framework survives ordinary per-frame failures and is destroyed only
// while no render/list/validation call can retain IDs or SDK-owned memory.
std::unique_ptr<FrameworkScope> framework;
std::unique_ptr<Engine> engine;

std::shared_lock<std::shared_mutex> acquireRuntime(bool needGpu) {
    for (;;) {
        std::shared_lock<std::shared_mutex> shared(rendererLifecycle);
        if (framework && (!needGpu || (engine && !engine->deviceFailed()))) return shared;
        shared.unlock();
        std::unique_lock<std::shared_mutex> exclusive(rendererLifecycle);
        if (engine && engine->deviceFailed()) engine.reset();
        if (!framework) framework = std::make_unique<FrameworkScope>();
        if (needGpu && !engine) engine = std::make_unique<Engine>();
    }
}

void validate(const RenderRequest& request) {
    if (request.width <= 0 || request.height <= 0 || request.width > kMaxDimension || request.height > kMaxDimension ||
        static_cast<std::uint64_t>(request.width) * request.height * 4 > kMaxFileBytes)
        throw std::runtime_error("Output dimensions exceed the renderer's 512 MiB pixel limit.");
    if (!std::isfinite(request.seconds) || !std::isfinite(request.secondsB) || !std::isfinite(request.blend) ||
        !std::isfinite(request.expressionWeightA) || !std::isfinite(request.expressionWeightB) ||
        !std::isfinite(request.mouthOpen) || request.mouthOpen < 0 || request.mouthOpen > 1 ||
        request.expressionWeightA < 0 || request.expressionWeightA > 1 || request.expressionWeightB < 0 || request.expressionWeightB > 1 ||
        request.blend < 0 || request.blend > 1.1f || !std::isfinite(request.scale) || !std::isfinite(request.offsetX) ||
        !std::isfinite(request.offsetY) || !std::isfinite(request.pixelAspect) || request.scale < 0 ||
        request.pixelAspect <= 0 || request.pixelAspect > 100)
        throw std::runtime_error("Invalid motion time, blend, expression weight, mouth opening, scale, position, or pixel aspect ratio.");
}
} // namespace

RenderResult render(const RenderRequest& request) {
    validate(request);
    ComScope com;
    frameworkError.clear();
    auto lifetime = acquireRuntime(true);
    return engine->render(request);
}

RendererStats rendererStats() {
    std::shared_lock<std::shared_mutex> lifetime(rendererLifecycle);
    std::lock_guard<std::mutex> lock(sdkMutex);
    return engine ? engine->stats() : RendererStats{};
}

std::vector<MotionEntry> listMotions(const std::wstring& modelPath) {
    frameworkError.clear();
    auto lifetime = acquireRuntime(false);
    std::lock_guard<std::mutex> lock(sdkMutex);
    const auto path = fullPath(modelPath);
    auto settings = modelSettings(path);
    std::vector<MotionEntry> result;
    for (int group = 0; group < settings->GetMotionGroupCount(); ++group) {
        const char* name = settings->GetMotionGroupName(group);
        for (int index = 0; index < settings->GetMotionCount(name); ++index) {
            const auto motion = referencedFile(path, settings->GetMotionFileName(name, index));
            result.push_back({wide(name) + L" / " + std::to_wstring(index + 1) + L" — " + motion.filename().wstring(), motion.wstring()});
        }
    }
    checkFramework();
    return result;
}

std::vector<MotionEntry> listExpressions(const std::wstring& modelPath) {
    frameworkError.clear();
    auto lifetime = acquireRuntime(false);
    std::lock_guard<std::mutex> lock(sdkMutex);
    const auto path = fullPath(modelPath);
    auto settings = modelSettings(path);
    std::vector<MotionEntry> result;
    for (int i = 0; i < settings->GetExpressionCount(); ++i) {
        const auto expression = referencedFile(path, settings->GetExpressionFileName(i));
        const auto name = wide(settings->GetExpressionName(i));
        result.push_back({name.empty() ? expression.filename().wstring() : name, expression.wstring()});
    }
    checkFramework();
    return result;
}

void validateExpression(const std::wstring& expressionPath) {
    frameworkError.clear();
    auto lifetime = acquireRuntime(false);
    std::lock_guard<std::mutex> lock(sdkMutex);
    readExpression(fullPath(expressionPath));
    checkFramework();
}

double motionDuration(const std::wstring& motionPath) {
    frameworkError.clear();
    auto lifetime = acquireRuntime(false);
    std::lock_guard<std::mutex> lock(sdkMutex);
    const auto bytes = normalizeMotionJson(readJson(fullPath(motionPath))).bytes;
    std::unique_ptr<Csm::CubismMotion, MotionDeleter> motion(
        Csm::CubismMotion::Create(bytes.data(), static_cast<int>(bytes.size()), nullptr, nullptr, true));
    if (!motion) throw std::runtime_error("Invalid motion3.json file.");
    const double duration = motion->GetLoopDuration();
    if (!std::isfinite(duration) || duration <= 0 || duration > 86400)
        throw std::runtime_error("Motion duration must be between zero and 86400 seconds.");
    checkFramework();
    return duration;
}

void releaseRenderer() {
    std::unique_lock<std::shared_mutex> lifetime(rendererLifecycle);
    engine.reset();
    framework.reset();
    frameworkError.clear();
}
} // namespace l2dae
