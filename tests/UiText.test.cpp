#include "UiText.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
void expect(bool condition, const char* explanation) {
    ++checks;
    if (!condition) throw std::runtime_error(explanation);
}
void equal(const std::string& source, const std::string& expected) {
    expect(l2dae::errorTextUtf8(source) == expected, "Incorrect Chinese error or damaged diagnostic context");
    expect(l2dae::errorTextUtf8(expected) == expected, "Chinese errors must be idempotent at nested UI boundaries");
    expect(l2dae::uiUtf8FromWide(l2dae::errorTextWide(source)) == expected, "Wide UI boundary changed UTF-8 error text");
}
}

int main() {
    try {
        equal("Import a Live2D model first.", u8"请先导入 Live2D 模型。");
        equal("This layer already contains animation clips. Create a new layer to import another model.",
            u8"此图层已有动作或表情片段。请新建图层，再导入其他模型。");
        equal("Invalid motion transition curve.", u8"回弹幅度设置无效，请重新选择。");
        equal("Live2D saved path is not terminated.", u8"已保存的 Live2D 路径数据不完整。请检查工程或重新导入素材。");
        equal("Cubism could not create a model instance.", u8"Cubism 无法创建模型实例。请检查模型文件或可用内存。");
        equal("After Effects did not supply the saved Live2D parameter. Remove and reapply the effect.",
            u8"AE 未提供已保存的 Live2D 参数。请移除该效果后重新添加。");
        equal("model3.json has no textures.", u8"model3.json 未声明纹理。请检查模型导出文件。");
        equal("Missing scripts/TimelineClips.jsx beside AeGOFlash.aex. Install the complete plugin folder.",
            u8"缺少 scripts/TimelineClips.jsx。请完整安装 AeGOFlash 文件夹，不能只复制 .aex。");
        equal("Invalid lip-sync audio format or sensitivity.",
            u8"声音同步口型的音频格式或灵敏度无效。请检查音频图层和口型灵敏度。");
        equal("Motion JSON: CurveCount does not match the curve array.",
            u8"动作文件格式错误：CurveCount 与实际曲线数量不一致。");
        equal("Motion JSON: malformed JSON near byte 2048.",
            u8"动作文件格式错误：JSON 格式无效，位置约为第 2048 字节。请检查动作文件。");
        equal("Motion JSON: missing or invalid TotalSegmentCount.",
            u8"动作文件格式错误：字段缺失或无效：TotalSegmentCount。");
        equal("Motion JSON: invalid UserDataCount.", u8"动作文件格式错误：字段无效：UserDataCount。");
        equal(u8"Motion JSON: duplicate object key: 表情😀.", u8"动作文件格式错误：JSON 包含重复字段：表情😀。");
        equal(u8"Motion JSON: curve 17 (Param腕_😀): truncated segment.",
            u8"动作文件格式错误：曲线 17 (Param腕_😀)：曲线段数据不完整。");
        equal("Motion JSON: curve 0 (Id): unexpected): unknown segment type; expected 0, 1, 2, or 3.",
            u8"动作文件格式错误：曲线 0 (Id): unexpected)：曲线段类型未知，只支持 0、1、2 或 3。");
        equal("Motion JSON: curve 2 (ParamPedalOn): unsupported infinite Bezier control value; first control value must equal the starting keyframe.",
            u8"动作文件格式错误：曲线 2 (ParamPedalOn)：不支持此无穷 Bezier 控制值：第一控制值必须等于起始关键帧值。请检查或重新导出动作。");
        const std::string path = u8R"(C:\模型😀\Cannot read: Motion JSON\表情.exp3.json)";
        equal("Cannot open: " + path, u8"无法打开文件，请检查路径和读取权限：" + path);
        equal("Cannot read: " + path, u8"无法读取文件，请检查文件和读取权限：" + path);
        equal("Empty or oversized file: " + path, u8"文件为空或超过支持的大小：" + path);
        equal("Invalid model3.json: " + path, u8"model3.json 模型描述无效：" + path);
        equal("Windows could not open the file picker: 12290", u8"Windows 无法打开文件选择窗口。错误码：12290");
        equal("After Effects does not provide AEGP Utility Suite.", u8"AE 未提供所需接口：AEGP Utility Suite。请检查 AE 版本。");
        equal("Opening the model texture failed (0x80070002).", u8"打开模型纹理失败，错误码：0x80070002。");
        equal("Direct3D rendering failed (0x887a0005).", u8"Direct3D 渲染失败，错误码：0x887a0005。");
        equal("Live2D: Invalid physics3.json file.", u8"Live2D 运行库：physics3.json 物理文件无效。请检查模型导出文件。");
        equal("AeGO Flash: Invalid motion transition curve.", u8"AeGO Flash：回弹幅度设置无效，请重新选择。");
        equal(u8"Error: AeGO Flash: 请先选择图层。", u8"错误：AeGO Flash：请先选择图层。");
        equal(u8"Error: 中文脚本错误：轨道😀", u8"错误：中文脚本错误：轨道😀");
        equal("new subsystem error 123", u8"操作未能完成。诊断信息：new subsystem error 123");
        equal(u8"Unknown read failure: C:\\模型😀\\文件.moc3", u8"操作未能完成。诊断信息：Unknown read failure: C:\\模型😀\\文件.moc3");
        equal("Motion JSON: new syntax detail 45", u8"动作文件格式错误：无法读取动作数据。诊断信息：new syntax detail 45");
        equal("", u8"操作未能完成。");

        const std::string payload = u8"A中😀文Z";
        expect(l2dae::uiUtf8FromWide(l2dae::uiWideFromUtf8(payload)) == payload, "Supplementary Unicode did not round trip");
        const std::string embedded("A\0B", 3);
        expect(l2dae::uiUtf8FromWide(l2dae::uiWideFromUtf8(embedded)) == embedded, "Length-aware conversion lost embedded NUL");
        const auto invalid = l2dae::uiUtf8FromWide(l2dae::uiWideFromUtf8(std::string("bad\xfftail", 8)));
        expect(invalid.find("bad") == 0 && invalid.find("tail") != std::string::npos, "Malformed UTF-8 discarded the whole diagnostic");
        for (size_t capacity = 0; capacity <= payload.size() + 3; ++capacity) {
            std::array<char, 32> storage{}; storage.fill('!');
            l2dae::copyUiUtf8(storage.data(), capacity, payload);
            expect(storage[capacity] == '!', "UTF-8 copy crossed the destination capacity");
            if (!capacity) { expect(storage[0] == '!', "Zero-capacity copy wrote a byte"); continue; }
            const std::string copied(storage.data());
            expect(copied.size() < capacity, "UTF-8 copy did not terminate within capacity");
            expect(payload.compare(0, copied.size(), copied) == 0, "UTF-8 copy changed source bytes");
            expect(l2dae::uiUtf8FromWide(l2dae::uiWideFromUtf8(copied)) == copied, "UTF-8 copy split a code point");
        }
        std::array<char, 256> host{};
        const std::string longPath = path + std::string(240, 'x') + u8"终😀";
        const auto longError = l2dae::errorTextUtf8("Cannot open: " + longPath);
        l2dae::copyUiUtf8(host.data(), host.size(), longError);
        expect(std::strlen(host.data()) < host.size(), "AE return message exceeded fixed field");
        expect(l2dae::uiUtf8FromWide(l2dae::uiWideFromUtf8(host.data())) == host.data(), "AE return message ended inside UTF-8");
        l2dae::copyUiUtf8(nullptr, 256, payload);
        std::cout << "PASS: " << checks << " UI error localization checks; Unicode paths/curve IDs, diagnostics, idempotence and fixed UTF-8 fields.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
