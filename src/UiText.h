#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "UiLanguage.h"
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

namespace l2dae {

// Copy into a fixed host field without cutting a UTF-8 code point. Input is
// already UTF-8; this helper does not convert it to the Windows/OEM code page.
inline void copyUiUtf8(char* destination, size_t capacity, std::string_view text) noexcept {
    if (!destination || !capacity) return;
    size_t count = text.size() < capacity ? text.size() : capacity - 1;
    if (count < text.size())
        while (count && (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80) --count;
    if (count) std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
}

// AE 2026 narrow UI fields use UTF-8. Win32 dialogs use the wide helper below;
// neither boundary should reinterpret paths or diagnostics as an OEM code page.
inline std::wstring uiWideFromUtf8(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return uiText(L"The error message is too long.", L"错误信息过长。");
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) return uiText(L"Unable to read the error message.", L"无法读取错误信息。");
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size))
        return uiText(L"Unable to read the error message.", L"无法读取错误信息。");
    return result;
}

inline std::string uiUtf8FromWide(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return simplifiedChineseUi() ? std::string(u8"错误信息过长。") : std::string("The error message is too long.");
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return simplifiedChineseUi() ? std::string(u8"无法读取错误信息。") : std::string("Unable to read the error message.");
    std::string result(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr))
        return simplifiedChineseUi() ? std::string(u8"无法读取错误信息。") : std::string("Unable to read the error message.");
    return result;
}

namespace ui_text_detail {
struct Translation { std::string_view source; std::string_view chinese; };

inline constexpr Translation messages[] = {
    {"Not enough memory.", u8"可用内存不足。请降低合成尺寸或释放内存后重试。"},
    {"bad allocation", u8"可用内存不足。请降低合成尺寸或释放内存后重试。"},
    {"Unexpected native rendering error.", u8"Live2D 渲染发生未预期的错误。请重新尝试，并保留出错时的模型与动作信息。"},
    {"Unexpected native timeline bridge error.", u8"添加时间线片段时发生未预期的错误。请检查时间线后重试。"},
    {"Unable to encode the effect UI text.", u8"无法编码效果面板文字。请检查 Windows 语言设置。"},
    {"The effect parameter name is too long.", u8"效果参数名称过长，无法显示。"},
    {"The saved Live2D timeline binding is invalid.", u8"已保存的 AeGO Flash 时间线关联无效。请在新模型图层上重新导入动作。"},
    {"Choose a Live2D model first.", u8"请先导入 Live2D 模型。"},
    {"Import a Live2D model first.", u8"请先导入 Live2D 模型。"},
    {"Select a model3.json file first.", u8"请先选择模型的 .model3.json 文件。"},
    {"This layer already contains animation clips. Create a new layer to import another model.", u8"此图层已有动作或表情片段。请新建图层，再导入其他模型。"},
    {"Import a Live2D model before importing motions or expressions.", u8"请先导入 Live2D 模型，再导入动作或表情。"},
    {"The default import transition must be a whole number of frames.", u8"默认过渡必须为整数帧。"},
    {"Choose a motion or expression to import.", u8"请选择要导入的动作或表情。"},
    {"Choose a motion, an expression, or both.", u8"请选择动作、表情，或同时选择两者。"},
    {"Choose one motion, one expression, or both for this import.", u8"每次请选择一个动作、一个表情，或同时选择两者。"},
    {"The default import transition is invalid.", u8"默认过渡设置无效。请输入 0–100000 之间的整数帧。"},
    {"The animation import model changed unexpectedly.", u8"导入动画时模型发生变化。请关闭导入窗口后重新打开。"},
    {"Animation import cannot replace an existing clip's saved source.", u8"不能用新素材替换已有片段保存的来源。请将新动作或表情导入为新片段。"},
    {"The selected animation entry is invalid.", u8"所选动作或表情无效。请重新选择素材。"},
    {"Live2D renderer returned an invalid frame size.", u8"Live2D 返回的画面尺寸无效。请重新渲染。"},
    {"A valid After Effects main window is required to open Live2D import.", u8"无法找到有效的 AE 主窗口。请在 AE 界面中打开 AeGO Flash 导入。"},
    {"Please choose a .model3.json file.", u8"请选择 .model3.json 模型文件。"},
    {"Please choose an .exp3.json file.", u8"请选择 .exp3.json 表情文件。"},
    {"Please choose a .motion3.json file.", u8"请选择 .motion3.json 动作文件。"},
    {"The selected motion has no valid duration.", u8"所选动作没有有效时长。请检查动作文件。"},
    {"The renderer returned an invalid preview image.", u8"渲染器返回的预览画面无效。请重新选择素材。"},
    {"Cannot populate the animation list.", u8"无法加载动作与表情列表。请关闭导入窗口后重试。"},
    {"This project has too many animation entries for After Effects.", u8"此工程的动画索引已超出 AE 可精确表示的范围。请拆分模型图层或工程。"},
    {"Transition frames must be between 0 and 100000.", u8"过渡帧数必须在 0–100000 之间。"},
    {"Enter a non-negative whole number of transition frames.", u8"请输入大于或等于 0 的整数过渡帧数。"},
    {"Cannot create Live2D import controls.", u8"无法创建 AeGO Flash 导入界面。请关闭窗口后重试。"},
    {"Cannot start the preview timer.", u8"无法启动预览播放。请关闭导入窗口后重试。"},
    {"The model file is missing. Restore its original path before importing.", u8"模型文件不存在。请恢复模型原路径后再导入。"},
    {"Windows could not create the Live2D import window.", u8"Windows 无法创建 AeGO Flash 导入窗口。请重试。"},
    {"Invalid lip-sync audio format or sensitivity.", u8"声音同步口型的音频格式或灵敏度无效。请检查音频图层和口型灵敏度。"},
    {"Invalid lip-sync audio buffer or window length.", u8"声音同步口型收到的音频数据或采样长度无效。请检查所选音频图层。"},
    {"Invalid Unicode path.", u8"文件路径包含无效字符。请重新选择文件。"},
    {"A model file reference is not valid UTF-8.", u8"模型中的文件引用不是有效 UTF-8。请检查模型导出的路径编码。"},
    {"Incomplete JSON Unicode escape.", u8"JSON 中的 Unicode 转义不完整。请检查文件内容。"},
    {"Invalid JSON Unicode escape.", u8"JSON 中的 Unicode 转义无效。请检查文件内容。"},
    {"Unpaired JSON Unicode surrogate.", u8"JSON 中的 Unicode 字符对不完整。请检查文件编码。"},
    {"Invalid JSON Unicode surrogate.", u8"JSON 中的 Unicode 字符对无效。请检查文件编码。"},
    {"Unsupported JSON control character.", u8"JSON 包含不支持的控制字符。请检查文件内容。"},
    {"A model file reference is empty.", u8"模型中的文件引用为空。请检查模型导出文件。"},
    {"Cannot locate the renderer module.", u8"无法定位 Live2D 渲染模块。请完整安装插件文件夹。"},
    {"Cannot start the Cubism Framework.", u8"无法启动 Cubism 运行库。请完整安装插件文件夹后重试。"},
    {"model3.json does not reference a .moc3 model.", u8"model3.json 未引用 .moc3 模型。请检查模型导出内容。"},
    {"Malformed expression JSON.", u8"表情文件的 JSON 格式无效。请检查 .exp3.json 文件。"},
    {"Expression numbers must be finite and representable as 32-bit values.", u8"表情数值必须是可用 32 位浮点数表示的有限值。"},
    {"Expression JSON contains duplicate object keys.", u8"表情 JSON 包含重复字段。请检查表情文件。"},
    {"Expression file exceeds 4 MiB.", u8"表情文件超过 4 MiB，无法读取。"},
    {"Expression must be a JSON object.", u8"表情文件最外层必须是 JSON 对象。"},
    {"Unsupported expression Type; expected Live2D Expression.", u8"不支持此表情 Type，应为 Live2D Expression。"},
    {"Expression Parameters must be an array with at most 4096 entries.", u8"表情 Parameters 必须是数组，且最多包含 4096 项。"},
    {"Expression fade times must be finite numbers.", u8"表情淡入、淡出时长必须是有限数值。"},
    {"Expression parameter Id must be a nonempty string of at most 1024 UTF-8 bytes.", u8"表情参数 Id 不能为空，且 UTF-8 长度不能超过 1024 字节。"},
    {"Expression contains duplicate parameter IDs.", u8"表情包含重复的参数 Id。请检查表情文件。"},
    {"Expression parameter Value must be a finite number.", u8"表情参数 Value 必须是有限数值。"},
    {"Numeric expression Blend must be Unity mode 0, 1, or 2.", u8"表情 Blend 数值只能使用 Unity 模式 0、1 或 2。"},
    {"Expression Blend must be Add, Multiply, or Overwrite.", u8"表情 Blend 必须为 Add、Multiply 或 Overwrite。"},
    {"Could not parse expression.", u8"无法解析表情。请检查 .exp3.json 文件。"},
    {"Model texture is too large.", u8"模型纹理尺寸无效或过大。请减小纹理后重新导出。"},
    {"Cubism could not create a model instance.", u8"Cubism 无法创建模型实例。请检查模型文件或可用内存。"},
    {"Expression result is outside the supported parameter range.", u8"表情计算结果超出支持的参数范围。请检查表情数值。"},
    {"This model has no supported mouth-opening parameter. Declare its parameter in the model3.json LipSync group.", u8"模型没有支持的嘴部开合参数。请在 model3.json 的 LipSync 组中声明对应参数。"},
    {"Invalid pose3.json file.", u8"pose3.json 姿态文件无效。请检查模型导出文件。"},
    {"Cannot create the Cubism D3D11 renderer.", u8"无法创建 Cubism D3D11 渲染器。请检查显卡驱动后重试。"},
    {"model3.json has no textures.", u8"model3.json 未声明纹理。请检查模型导出文件。"},
    {"Invalid or zero-duration motion3.json.", u8"motion3.json 动作无效或时长为 0。请检查动作文件。"},
    {"Invalid breathing amount.", u8"呼吸幅度无效。请检查呼吸幅度数值或表达式。"},
    {"Invalid blink strength.", u8"保存的眨眼强度无效。请检查旧工程中的眨眼设置。"},
    {"Invalid breathing period.", u8"呼吸周期无效。请检查呼吸周期数值或表达式。"},
    {"Invalid blink interval or duration.", u8"眨眼间隔或保存的眨眼时长无效。请检查眨眼设置。"},
    {"Invalid breathing or auto-blink layer time.", u8"呼吸或自动眨眼的图层时间无效。请检查时间设置。"},
    {"Invalid physics3.json file.", u8"physics3.json 物理文件无效。请检查模型导出文件。"},
    {"Cubism could not reset model geometry for independent frame evaluation.", u8"Cubism 无法重置模型几何状态，不能独立计算当前帧。请重新加载模型后重试。"},
    {"Could not clear the model drawing order.", u8"无法重置模型绘制顺序。请重新加载模型后重试。"},
    {"The transition produced an invalid model drawing order.", u8"动作过渡产生了无效绘制顺序。请检查这两个动作与模型的匹配关系。"},
    {"Invalid motion transition curve.", u8"回弹幅度设置无效，请重新选择。"},
    {"The model has invalid canvas dimensions.", u8"模型画布尺寸无效。请检查模型导出设置。"},
    {"Cubism could not load this MOC3 file; check its export version.", u8"Cubism 无法加载此 MOC3 文件，请检查模型的导出版本。"},
    {"Direct3D device recovery is pending.", u8"Direct3D 设备正在等待恢复。请稍后重新渲染。"},
    {"Output dimensions exceed the renderer's 512 MiB pixel limit.", u8"输出画面超出渲染器的 512 MiB 像素内存限制。请降低合成尺寸。"},
    {"Invalid motion time, blend, expression weight, mouth opening, scale, position, or pixel aspect ratio.", u8"动作时间、混合权重、表情权重、嘴部开合、缩放、位置或像素宽高比无效。请检查参数与表达式。"},
    {"Invalid motion3.json file.", u8"motion3.json 动作文件无效。请检查动作文件。"},
    {"Motion duration must be between zero and 86400 seconds.", u8"动作时长必须大于 0 且不超过 86400 秒。"},
    {"Live2D saved path has invalid UTF-8 encoding or excessive length.", u8"已保存的 Live2D 路径编码无效或过长。请重新选择素材。"},
    {"Live2D saved path is not terminated.", u8"已保存的 Live2D 路径数据不完整。请检查工程或重新导入素材。"},
    {"Cannot decode the Live2D file path.", u8"无法解码 Live2D 文件路径。请重新选择素材。"},
    {"Live2D file path is too long or contains a null character.", u8"Live2D 文件路径过长或包含空字符。请检查路径。"},
    {"Live2D file path contains invalid Unicode.", u8"Live2D 文件路径包含无效 Unicode 字符。请重新选择素材。"},
    {"Cannot encode the Live2D file path.", u8"无法编码 Live2D 文件路径。请重新选择素材。"},
    {"Unsupported legacy Live2D saved-data version.", u8"不支持此旧版 Live2D 保存数据。请使用兼容版本打开工程。"},
    {"Unsupported legacy Live2D expression-data version.", u8"不支持此旧版 Live2D 表情数据。请使用兼容版本打开工程。"},
    {"Live2D saved-data header is invalid.", u8"Live2D 保存数据头无效。请检查工程副本或重新导入模型。"},
    {"Live2D saved-data handle has an invalid size.", u8"Live2D 保存数据大小无效。请检查工程副本。"},
    {"Live2D expression-data handle has an invalid size.", u8"Live2D 表情数据大小无效。请检查工程副本。"},
    {"After Effects did not supply the saved Live2D parameter. Remove and reapply the effect.", u8"AE 未提供已保存的 Live2D 参数。请移除该效果后重新添加。"},
    {"Live2D saved data needs migration. Save and reopen the project with this plug-in, then import again.", u8"Live2D 保存数据需要迁移。请用此插件保存并重新打开工程，再次导入。"},
    {"Live2D saved path data is too large.", u8"Live2D 保存的路径数据过大。请将素材分到不同图层或工程。"},
    {"Live2D animation index exceeds host numeric precision.", u8"Live2D 动画索引超出 AE 的数值精度范围。请拆分模型图层或工程。"},
    {"Timeline clips require an interactive After Effects session.", u8"添加时间线片段需要打开 AE 界面，请在 AE 中执行导入。"},
    {"After Effects scripting is unavailable. Timeline clips could not be added.", u8"AE 脚本功能不可用，未能添加时间线片段。请检查 AE 脚本支持后重试。"},
    {"Cannot locate the Live2D plugin module.", u8"无法定位 AeGO Flash 插件模块。请完整安装插件文件夹。"},
    {"Cannot read the Live2D plugin installation path.", u8"无法读取 AeGO Flash 插件安装路径。请检查安装位置。"},
    {"Live2D plugin installation path is too long.", u8"AeGO Flash 插件安装路径过长。请使用较短的安装路径。"},
    {"Missing scripts/TimelineClips.jsx beside AeGOFlash.aex. Install the complete plugin folder.", u8"缺少 scripts/TimelineClips.jsx。请完整安装 AeGOFlash 文件夹，不能只复制 .aex。"},
    {"TimelineClips.jsx is empty or exceeds the supported size.", u8"TimelineClips.jsx 为空或超过支持的大小。请完整替换插件文件夹。"},
    {"Cannot read scripts/TimelineClips.jsx.", u8"无法读取 scripts/TimelineClips.jsx。请检查安装文件与读取权限。"},
    {"TimelineClips.jsx must be a UTF-8 script without embedded NUL bytes.", u8"TimelineClips.jsx 必须为不含空字符的 UTF-8 脚本。请使用发布包内的原文件。"},
    {"Clip label is too long or contains invalid Unicode.", u8"片段名称过长或包含无效 Unicode 字符。请检查素材名称。"},
    {"Invalid Live2D timeline clip request.", u8"AeGO Flash 时间线片段请求无效。请重新选择动作或表情导入。"},
    {"Cannot read the After Effects script result.", u8"无法读取 AE 脚本执行结果。请检查时间线后重试。"},
    {"Cannot lock the After Effects script result.", u8"无法访问 AE 脚本执行结果。请检查时间线后重试。"},
    {"After Effects failed to execute TimelineClips.jsx.", u8"AE 未能执行 TimelineClips.jsx。请检查插件脚本文件后重试。"},
    {"TimelineClips.jsx did not confirm clip creation. Check the target effect still exists, then import the clip again.", u8"脚本未确认片段创建成功。请检查目标效果仍存在，再重新导入片段。"},
    {"This host does not provide the After Effects timeline scripting bridge.", u8"当前宿主不提供 AE 时间线脚本接口。请在受支持的 AE 中导入片段。"},
    {"Timeline clips must be imported on the After Effects main thread.", u8"时间线片段必须从 AE 主界面导入。请在效果面板中重新操作。"},
    {"Required After Effects timeline callbacks are unavailable.", u8"所需的 AE 时间线接口不可用。请检查 AE 版本后重试。"},
    {"Cannot register the Live2D timeline bridge with After Effects.", u8"无法向 AE 注册 AeGO Flash 时间线接口。请重新启动 AE 后重试。"},
    {"Cannot register the Live2D timeline idle callback.", u8"无法注册 AeGO Flash 时间线任务。请重新启动 AE 后重试。"},
    {"Live2D imports must run on the After Effects main thread.", u8"AeGO Flash 导入必须从 AE 主界面运行。请在效果面板中重新操作。"},
    {"Cannot obtain the After Effects main window. Live2D import was not opened.", u8"无法取得 AE 主窗口，AeGO Flash 导入未打开。请在 AE 主界面中重试。"},
    {"Cannot create a unique timeline clip binding.", u8"无法创建唯一的时间线片段关联。请重新导入。"},
    {"Cannot generate a new timeline clip binding. Try importing again.", u8"无法生成新的时间线片段关联。请重新导入。"},
    {"Live2D is still adding earlier clips. Wait and try again.", u8"AeGO Flash 正在添加之前的片段，请稍候再导入。"},
};

inline constexpr Translation motionMessages[] = {
    {"unsupported string control character.", u8"字符串包含不支持的控制字符。"},
    {"numbers must be finite and representable as 32-bit values.", u8"数值必须是可用 32 位浮点数表示的有限值。"},
    {"Infinity is supported only for a stepped incoming Bezier tangent; invalid value elsewhere.", u8"Infinity 只支持已识别的阶梯式 Bezier 控制点模式，其他位置不能使用无穷值。"},
    {"normalized file is too large.", u8"规范化后的动作文件过大。"},
    {"file must be nonempty and at most 64 MiB.", u8"动作文件不能为空，且不能超过 64 MiB。"},
    {"Version must be 3.", u8"动作文件 Version 必须为 3。"},
    {"Duration must be greater than zero and at most 86400 seconds.", u8"Duration 必须大于 0 且不超过 86400 秒。"},
    {"Fps must be greater than zero.", u8"Fps 必须大于 0。"},
    {"CurveCount does not match the curve array.", u8"CurveCount 与实际曲线数量不一致。"},
    {"unsupported curve Target.", u8"不支持此曲线 Target。"},
    {"curve Id must contain 1 to 1024 UTF-8 bytes.", u8"曲线 Id 的 UTF-8 长度必须为 1–1024 字节。"},
    {"Segments is truncated or contains no segment.", u8"Segments 数据不完整或没有曲线段。"},
    {"initial time and value must be finite, with nonnegative time.", u8"起始时间和值必须为有限数值，且时间不能小于 0。"},
    {"unknown segment type; expected 0, 1, 2, or 3.", u8"曲线段类型未知，只支持 0、1、2 或 3。"},
    {"truncated segment.", u8"曲线段数据不完整。"},
    {"segment times and values must be finite numbers.", u8"曲线段的时间和值必须为有限数值。"},
    {"segment endpoint times must increase at 32-bit precision.", u8"曲线段终点时间必须在 32 位精度下严格递增。"},
    {"unsupported infinite Bezier control value; first control value must equal the starting keyframe.", u8"不支持此无穷 Bezier 控制值：第一控制值必须等于起始关键帧值。请检查或重新导出动作。"},
    {"infinite-tangent Bezier control times must be ordered within the endpoints.", u8"无穷切线 Bezier 的控制时间必须在起止点之间按顺序排列。"},
    {"too many segments or points.", u8"曲线段或控制点数量超出支持范围。"},
    {"TotalSegmentCount does not match the segments.", u8"TotalSegmentCount 与实际曲线段数量不一致。"},
    {"TotalPointCount does not match the original points.", u8"TotalPointCount 与原始控制点数量不一致。"},
    {"UserData must be an array.", u8"UserData 必须为数组。"},
    {"UserDataCount does not match UserData.", u8"UserDataCount 与 UserData 项数不一致。"},
    {"UserData time must be nonnegative.", u8"UserData 的时间不能小于 0。"},
};

inline constexpr Translation operations[] = {
    {"COM initialization", u8"初始化 Windows COM"},
    {"Locating the renderer module", u8"定位渲染模块"},
    {"Creating the PNG decoder", u8"创建 PNG 解码器"},
    {"Opening the model texture", u8"打开模型纹理"},
    {"Reading the model texture", u8"读取模型纹理"},
    {"Reading texture dimensions", u8"读取纹理尺寸"},
    {"Creating the texture converter", u8"创建纹理转换器"},
    {"Converting the model texture", u8"转换模型纹理"},
    {"Decoding the model texture", u8"解码模型纹理"},
    {"Uploading the model texture", u8"上传模型纹理"},
    {"Binding the model texture", u8"绑定模型纹理"},
    {"Creating a Direct3D 11 device", u8"创建 Direct3D 11 设备"},
    {"Creating the offscreen target", u8"创建离屏渲染目标"},
    {"Creating the offscreen view", u8"创建离屏视图"},
    {"Creating the pixel readback buffer", u8"创建像素读回缓冲"},
    {"Reading the rendered pixels", u8"读取渲染像素"},
    {"Direct3D rendering", u8"Direct3D 渲染"},
};

inline constexpr Translation dataPrefixes[] = {
    {"Cannot open: ", u8"无法打开文件，请检查路径和读取权限："},
    {"Cannot read: ", u8"无法读取文件，请检查文件和读取权限："},
    {"Empty or oversized file: ", u8"文件为空或超过支持的大小："},
    {"Invalid model3.json: ", u8"model3.json 模型描述无效："},
    {"Windows could not open the file picker: ", u8"Windows 无法打开文件选择窗口。错误码："},
};

template<size_t N>
inline std::string_view lookup(std::string_view text, const Translation (&entries)[N]) {
    for (const auto& entry : entries) if (entry.source == text) return entry.chinese;
    return {};
}
inline bool starts(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}
inline bool ends(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}
inline std::string append(std::string_view prefix, std::string_view payload, std::string_view suffix = {}) {
    std::string result(prefix); result.append(payload); result.append(suffix); return result;
}

inline std::string motionText(std::string_view text) {
    if (const auto known = lookup(text, motionMessages); !known.empty()) return std::string(known);
    if (starts(text, "curve ")) {
        const auto delimiter = text.rfind("): ");
        // Preserve the numeric index and complete parameter Id, including punctuation.
        if (delimiter != std::string_view::npos) {
            const auto reason = text.substr(delimiter + 3);
            if (const auto known = lookup(reason, motionMessages); !known.empty())
                return append(u8"曲线 ", text.substr(6, delimiter - 5), u8"：") + std::string(known);
        }
    }
    if (starts(text, "malformed JSON near byte ") && ends(text, "."))
        return append(u8"JSON 格式无效，位置约为第 ", text.substr(25, text.size() - 26), u8" 字节。请检查动作文件。");
    if (starts(text, "duplicate object key: ") && ends(text, "."))
        return append(u8"JSON 包含重复字段：", text.substr(22, text.size() - 23), u8"。");
    if (starts(text, "missing or invalid ") && ends(text, "."))
        return append(u8"字段缺失或无效：", text.substr(19, text.size() - 20), u8"。");
    if (starts(text, "invalid ") && ends(text, "."))
        return append(u8"字段无效：", text.substr(8, text.size() - 9), u8"。");
    return append(u8"无法读取动作数据。诊断信息：", text);
}

inline std::string translated(std::string_view text, unsigned depth = 0) {
    if (text.empty()) return u8"操作未能完成。";
    if (const auto known = lookup(text, messages); !known.empty()) return std::string(known);
    // Some translated messages start with identifiers such as AE, Live2D or
    // model3.json. Recognize our actual outputs, never merely the presence of
    // Chinese characters: an untranslated English error can contain a Chinese path.
    for (const auto& entry : messages) if (text == entry.chinese) return std::string(text);
    for (const auto& entry : dataPrefixes) if (starts(text, entry.chinese)) return std::string(text);
    if (starts(text, u8"AE 未提供所需接口：") || starts(text, u8"Live2D 运行库：") ||
        starts(text, u8"AeGO Flash：")) return std::string(text);
    for (const auto& entry : operations)
        if (starts(text, entry.chinese) && starts(text.substr(entry.chinese.size()), u8"失败，错误码："))
            return std::string(text);
    if (starts(text, "Motion JSON: ")) return append(u8"动作文件格式错误：", motionText(text.substr(13)));
    // These suffixes are data, not messages. Never replace words inside a path.
    for (const auto& entry : dataPrefixes)
        if (starts(text, entry.source)) return append(entry.chinese, text.substr(entry.source.size()));
    if (starts(text, "After Effects does not provide ") && ends(text, "."))
        return append(u8"AE 未提供所需接口：", text.substr(31, text.size() - 32), u8"。请检查 AE 版本。");
    const auto failed = text.find(" failed (0x");
    if (failed != std::string_view::npos && ends(text, ").")) {
        const auto operation = lookup(text.substr(0, failed), operations);
        if (!operation.empty())
            return append(operation, u8"失败，错误码：") + std::string(text.substr(failed + 9, text.size() - failed - 11)) + u8"。";
    }
    if (depth < 8) {
        if (starts(text, "AeGO Flash: ")) return append(u8"AeGO Flash：", translated(text.substr(12), depth + 1));
        if (starts(text, "Live2D: ")) return append(u8"Live2D 运行库：", translated(text.substr(8), depth + 1));
        if (starts(text, "Error: ")) return append(u8"错误：", translated(text.substr(7), depth + 1));
    }
    // Already localized errors (including messages from the JSX) pass through.
    // A non-ASCII path alone is also kept as an unmodified diagnostic payload.
    if (static_cast<unsigned char>(text.front()) >= 0x80) return std::string(text);
    return append(u8"操作未能完成。诊断信息：", text);
}
} // namespace ui_text_detail

inline std::string errorTextUtf8(const std::string& text) {
    if (!simplifiedChineseUi())
        return text.empty() ? std::string("The operation could not be completed.") : text;
    return ui_text_detail::translated(text);
}
inline std::wstring errorTextWide(const std::string& text) { return uiWideFromUtf8(errorTextUtf8(text)); }

} // namespace l2dae
