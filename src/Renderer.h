#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace l2dae {
struct MotionEntry {
    std::wstring label;
    std::wstring path;
};
struct RenderRequest {
    std::wstring modelPath;
    std::wstring motionPath;
    double seconds = 0;
    bool loop = true;
    int width = 0;
    int height = 0;
    float pixelAspect = 1; // Physical width/height of each requested output pixel.
    float scale = 1;
    float offsetX = 0; // Pixels in the requested output.
    float offsetY = 0;
    std::wstring motionPathB; // Empty selects the model's static pose for source B.
    double secondsB = 0;     // Independent local motion time, including time remapping.
    float blend = 0;         // 0 = A, 1 = B; up to 1.1 extrapolates real deformation
                            // parameters past B, clamped to each model range.
                            // Part visibility, opacity and drawing order stop at B.
    std::wstring expressionPathA;
    std::wstring expressionPathB;
    float expressionWeightA = 0; // Expression strength; timeline owns fades.
    float expressionWeightB = 0;
    bool lipSyncEnabled = false;
    float mouthOpen = 0; // Audio envelope in [0,1]; overrides mouth opening, preserving shape.
    bool breathingEnabled = false;
    bool autoBlinkEnabled = false;
    double ambientSeconds = 0; // Independent layer clock; motion time remapping does not affect it.
    float breathingAmount = 1; // Fraction of the model's breath range; zero leaves its pose alone.
    double breathingPeriod = 4;
    float blinkStrength = 1; // Closure strength; closed expressions never reopen.
    double blinkInterval = 4;
    double blinkDuration = 0.3;
};
struct RenderResult {
    int width = 0;
    int height = 0;
    // Top-down, premultiplied RGBA, 8 bits per component.
    std::vector<std::uint8_t> rgba;
};
// Frames use a bounded pool of independent CPU models; GPU submission is serialized.
// Time is evaluated independently of call order.
RenderResult render(const RenderRequest& request);
struct RendererStats {
    std::uint32_t workerLimit = 0;
    std::uint32_t workerCount = 0;
    std::uint32_t modelInstances = 0;
    std::uint32_t activeCpuEvaluations = 0;
    std::uint32_t peakCpuConcurrency = 0;
    std::uint64_t sharedTextureBytes = 0;
    std::uint64_t sharedModelBytes = 0;
    std::uint64_t framesRendered = 0;
};
RendererStats rendererStats(); // Diagnostics only; does not create a renderer.
std::vector<MotionEntry> listMotions(const std::wstring& modelPath);
std::vector<MotionEntry> listExpressions(const std::wstring& modelPath);
void validateExpression(const std::wstring& expressionPath);
double motionDuration(const std::wstring& motionPath);
void releaseRenderer();
}
