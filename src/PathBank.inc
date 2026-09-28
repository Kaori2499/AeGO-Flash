#pragma once
// Host handles are fixed-size POD wrappers. Owned immutable banks live outside
// movable AE memory; snapshots may be shared across MFR calls, undo and copies.
#include <memory>
#include <vector>
#include <mutex>

constexpr size_t kPathCapacity = 8192; // Legacy fixed path storage only.
constexpr size_t kLegacySlots = 8;
constexpr size_t kMaximumWireBytes = 256u * 1024u * 1024u;
constexpr size_t kMaximumPathBytes = 131068; // Windows maximum Unicode path.
constexpr size_t kMaximumIndex = 16777216; // Largest consecutive float integer.
struct SelectionData { std::string model; std::vector<std::string> motions; };
struct ExpressionData { std::vector<std::string> paths; };
struct LegacySelectionData { std::uint32_t version; char model[kPathCapacity]; char motion[kPathCapacity]; };
struct LegacySelectionData2 { std::uint32_t version; char model[kPathCapacity]; char motions[kLegacySlots][kPathCapacity]; };
struct LegacyExpressionData { std::uint32_t version; char paths[kLegacySlots][kPathCapacity]; };
constexpr std::uint32_t kBankMagic = 0x3342324c;
template<class T> struct BankHolder { std::shared_ptr<const T> snapshot; };
struct BankHeader { std::uint32_t magic; std::uint32_t kind; void* holder; };
static_assert(std::is_trivially_copyable_v<BankHeader>);
std::mutex bankCommitMutex; // Paired model/expression snapshot/commit only.
template<class T> constexpr std::uint32_t bankKind() { return std::is_same_v<T, SelectionData> ? 102 : 117; }

void validatePath(const std::string& value) {
    if (value.size() > kMaximumPathBytes || value.find('\0') != std::string::npos ||
        (!value.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0)))
        throw std::runtime_error("Live2D saved path has invalid UTF-8 encoding or excessive length.");
}
std::string legacyPath(const char* value) {
    const auto end = static_cast<const char*>(std::memchr(value, 0, kPathCapacity));
    if (!end) throw std::runtime_error("Live2D saved path is not terminated.");
    std::string result(value, end); validatePath(result); return result;
}
std::wstring widePath(const std::string& value) {
    validatePath(value); if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count))
        throw std::runtime_error("Cannot decode the Live2D file path.");
    return result;
}
void storePath(const std::wstring& value, std::string& target) {
    if (value.size() > 32767 || value.find(L'\0') != std::wstring::npos)
        throw std::runtime_error("Live2D file path is too long or contains a null character.");
    if (value.empty()) { target.clear(); return; }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0 || count > kMaximumPathBytes) throw std::runtime_error("Live2D file path contains invalid Unicode.");
    std::string encoded(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), encoded.data(), count, nullptr, nullptr))
        throw std::runtime_error("Cannot encode the Live2D file path.");
    target = std::move(encoded);
}
SelectionData migrateLegacy(const LegacySelectionData& value) {
    if (value.version != 1) throw std::runtime_error("Unsupported legacy Live2D saved-data version.");
    SelectionData result; result.model = legacyPath(value.model);
    result.motions.resize(kLegacySlots); result.motions[0] = legacyPath(value.motion); return result;
}
SelectionData migrateLegacy(const LegacySelectionData2& value) {
    if (value.version != 2) throw std::runtime_error("Unsupported legacy Live2D saved-data version.");
    SelectionData result; result.model = legacyPath(value.model);
    for (const auto& path : value.motions) result.motions.push_back(legacyPath(path)); return result;
}
ExpressionData migrateLegacy(const LegacyExpressionData& value) {
    if (value.version != 1) throw std::runtime_error("Unsupported legacy Live2D expression-data version.");
    ExpressionData result; for (const auto& path : value.paths) result.paths.push_back(legacyPath(path)); return result;
}

template<class T> std::shared_ptr<const T> readBank(PF_InData* in_data, PF_ArbitraryH handle) {
    if (!handle) return std::make_shared<const T>();
    const auto size = PF_GET_HANDLE_SIZE(handle);
    const auto* bytes = PF_LOCK_HANDLE(handle);
    if (!bytes) throw static_cast<PF_Err>(PF_Err_OUT_OF_MEMORY);
    try {
        std::shared_ptr<const T> result;
        if (size == sizeof(BankHeader)) {
            BankHeader header; std::memcpy(&header, bytes, sizeof(header));
            if (header.magic != kBankMagic || header.kind != bankKind<T>() || !header.holder)
                throw std::runtime_error("Live2D saved-data header is invalid.");
            result = std::atomic_load(&static_cast<BankHolder<T>*>(header.holder)->snapshot);
        } else if constexpr (std::is_same_v<T, SelectionData>) {
            if (size == sizeof(LegacySelectionData)) result = std::make_shared<const T>(migrateLegacy(*static_cast<const LegacySelectionData*>(bytes)));
            else if (size == sizeof(LegacySelectionData2)) result = std::make_shared<const T>(migrateLegacy(*static_cast<const LegacySelectionData2*>(bytes)));
            else throw std::runtime_error("Live2D saved-data handle has an invalid size.");
        } else {
            if (size != sizeof(LegacyExpressionData)) throw std::runtime_error("Live2D expression-data handle has an invalid size.");
            result = std::make_shared<const T>(migrateLegacy(*static_cast<const LegacyExpressionData*>(bytes)));
        }
        PF_UNLOCK_HANDLE(handle); return result;
    } catch (...) { PF_UNLOCK_HANDLE(handle); throw; }
}
auto readSelection(PF_InData* in, PF_ArbitraryH handle) { return readBank<SelectionData>(in, handle); }
auto readExpression(PF_InData* in, PF_ArbitraryH handle) { return readBank<ExpressionData>(in, handle); }

template<class T> PF_ArbitraryH newBank(PF_InData* in_data, std::shared_ptr<const T> value) {
    auto holder = std::make_unique<BankHolder<T>>(); holder->snapshot = std::move(value);
    const BankHeader header{kBankMagic, bankKind<T>(), holder.get()};
    const auto handle = PF_NEW_HANDLE(sizeof(header));
    if (!handle) throw static_cast<PF_Err>(PF_Err_OUT_OF_MEMORY);
    auto* bytes = PF_LOCK_HANDLE(handle);
    if (!bytes) { PF_DISPOSE_HANDLE(handle); throw static_cast<PF_Err>(PF_Err_OUT_OF_MEMORY); }
    std::memcpy(bytes, &header, sizeof(header)); PF_UNLOCK_HANDLE(handle); holder.release(); return handle;
}
template<class T> void disposeBank(PF_InData* in_data, PF_ArbitraryH handle) {
    if (!handle) return;
    if (PF_GET_HANDLE_SIZE(handle) == sizeof(BankHeader)) {
        auto* bytes = PF_LOCK_HANDLE(handle);
        if (!bytes) throw static_cast<PF_Err>(PF_Err_OUT_OF_MEMORY);
        BankHeader header; std::memcpy(&header, bytes, sizeof(header)); PF_UNLOCK_HANDLE(handle);
        if (header.magic != kBankMagic || header.kind != bankKind<T>() || !header.holder)
            throw std::runtime_error("Live2D saved-data header is invalid.");
        delete static_cast<BankHolder<T>*>(header.holder);
    }
    PF_DISPOSE_HANDLE(handle);
}

// AE may keep an outer lock on these borrowed handles. No import changes their
// allocation, address or header, nor frees them; only the immutable bank changes.
template<class T> class ParameterWriteLock {
public:
    ParameterWriteLock(PF_InData* in, PF_ArbitraryH handle) : in_(in), handle_(handle) {
        if (!handle_) throw std::runtime_error("After Effects did not supply the saved Live2D parameter. Remove and reapply the effect.");
        if (in_->utils->host_get_handle_size(handle_) != sizeof(BankHeader))
            throw std::runtime_error("Live2D saved data needs migration. Save and reopen the project with this plug-in, then import again.");
        const auto* bytes = in_->utils->host_lock_handle(handle_);
        if (!bytes) throw static_cast<PF_Err>(PF_Err_OUT_OF_MEMORY);
        BankHeader header; std::memcpy(&header, bytes, sizeof(header));
        if (header.magic != kBankMagic || header.kind != bankKind<T>() || !header.holder) {
            in_->utils->host_unlock_handle(handle_); throw std::runtime_error("Live2D saved-data header is invalid.");
        }
        holder_ = static_cast<BankHolder<T>*>(header.holder);
    }
    ~ParameterWriteLock() { if (holder_) in_->utils->host_unlock_handle(handle_); }
    void write(std::shared_ptr<const T> value) noexcept { std::atomic_store(&holder_->snapshot, std::move(value)); }
    ParameterWriteLock(const ParameterWriteLock&) = delete;
    ParameterWriteLock& operator=(const ParameterWriteLock&) = delete;
private:
    PF_InData* in_; PF_ArbitraryH handle_; BankHolder<T>* holder_ = nullptr;
};

template<class T> const std::vector<std::string>& pathsOf(const T& value) {
    if constexpr (std::is_same_v<T, SelectionData>) return value.motions; else return value.paths;
}
template<class T> std::vector<std::string>& pathsOf(T& value) {
    if constexpr (std::is_same_v<T, SelectionData>) return value.motions; else return value.paths;
}
template<class T> const char* wireMagic() { return std::is_same_v<T, SelectionData> ? "L2DAE003" : "L2DEX002"; }
template<class T> constexpr std::uint32_t wireVersion() { return std::is_same_v<T, SelectionData> ? 3 : 2; }
template<class T> size_t wireSize(const T& value) {
    size_t total = 16;
    const auto add = [&](const std::string& path) { validatePath(path); if (path.size() + 4 > kMaximumWireBytes - total) throw std::runtime_error("Live2D saved path data is too large."); total += 4 + path.size(); };
    const auto& paths = pathsOf(value);
    if (paths.size() > kMaximumIndex) throw std::runtime_error("Live2D animation index exceeds host numeric precision.");
    if constexpr (std::is_same_v<T, SelectionData>) add(value.model);
    for (const auto& path : paths) add(path);
    return total;
}
void write32(unsigned char*& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) *out++ = static_cast<unsigned char>(value >> (8 * i));
}
template<class T> void flattenBank(const T& value, void* target, size_t size) {
    if (!target || size < wireSize(value)) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
    auto* out = static_cast<unsigned char*>(target); std::memcpy(out, wireMagic<T>(), 8); out += 8;
    write32(out, wireVersion<T>()); write32(out, static_cast<std::uint32_t>(pathsOf(value).size()));
    const auto append = [&](const std::string& path) { write32(out, static_cast<std::uint32_t>(path.size())); std::memcpy(out, path.data(), path.size()); out += path.size(); };
    if constexpr (std::is_same_v<T, SelectionData>) append(value.model);
    for (const auto& path : pathsOf(value)) append(path);
}
template<class T> T unflattenBank(const void* source, size_t size) {
    if (!source || size < 12 || size > kMaximumWireBytes) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
    const auto* bytes = static_cast<const unsigned char*>(source);
    if constexpr (std::is_same_v<T, SelectionData>) {
        if (size == 8 + sizeof(LegacySelectionData) && !std::memcmp(bytes, "L2DAE001", 8)) { LegacySelectionData value; std::memcpy(&value, bytes + 8, sizeof(value)); return migrateLegacy(value); }
        if (size == 8 + sizeof(LegacySelectionData2) && !std::memcmp(bytes, "L2DAE002", 8)) { LegacySelectionData2 value; std::memcpy(&value, bytes + 8, sizeof(value)); return migrateLegacy(value); }
    } else if (size == 8 + sizeof(LegacyExpressionData) && !std::memcmp(bytes, "L2DEX001", 8)) { LegacyExpressionData value; std::memcpy(&value, bytes + 8, sizeof(value)); return migrateLegacy(value); }
    if (std::memcmp(bytes, wireMagic<T>(), 8)) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
    size_t at = 8;
    const auto read32 = [&]() { if (size - at < 4) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM); std::uint32_t value = 0; for (unsigned i=0; i<4; ++i) value |= static_cast<std::uint32_t>(bytes[at++]) << (8*i); return value; };
    if (read32() != wireVersion<T>()) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
    const size_t count = read32();
    if (count > kMaximumIndex || count > (size - at) / 4) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
    const auto path = [&]() { const size_t length = read32(); if (length > kMaximumPathBytes || length > size-at) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM); std::string value(reinterpret_cast<const char*>(bytes+at), length); at += length; validatePath(value); return value; };
    T value; if constexpr (std::is_same_v<T, SelectionData>) value.model = path();
    auto& paths = pathsOf(value); paths.reserve(count);
    for (size_t i=0; i<count; ++i) paths.push_back(path());
    if (at != size) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
    return value;
}
std::string hexText(const std::string& path) {
    constexpr char digits[] = "0123456789abcdef"; std::string result; result.reserve(path.size()*2);
    for (const auto c : path) { const auto b = static_cast<unsigned char>(c); result += digits[b >> 4]; result += digits[b & 15]; } return result;
}
int hexDigit(char c) { if (c>='0' && c<='9') return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10;return -1; }
std::string parseHex(const std::string& text, size_t begin, size_t end) {
    if ((end-begin)%2 || (end-begin)/2 > kMaximumPathBytes) throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
    std::string result; result.reserve((end-begin)/2);
    for(size_t i=begin;i<end;i+=2) { const int a=hexDigit(text[i]), b=hexDigit(text[i+1]); if(a<0||b<0||!(a|b)) throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT); result += static_cast<char>((a<<4)|b); }
    validatePath(result); return result;
}
template<class T> const char* printPrefix() { return std::is_same_v<T,SelectionData> ? "Live2DNative3|" : "Live2DExpression2|"; }
template<class T> std::string printableBank(const T& value) {
    (void)wireSize(value);
    std::string result = printPrefix<T>(); result += std::to_string(pathsOf(value).size());
    if constexpr (std::is_same_v<T,SelectionData>) result += "|" + hexText(value.model);
    for(const auto& path:pathsOf(value)) result += "|"+hexText(path); return result;
}
template<class T> T scanBank(const char* source, size_t size) {
    try {
        if(!source || size > kMaximumWireBytes*2+128) throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
        std::string text(source,size); while(!text.empty()&&(text.back()=='\0'||text.back()=='\r'||text.back()=='\n'))text.pop_back();
        size_t at = 0, count = 0; bool legacyOne = false;
        if(text.compare(0,std::strlen(printPrefix<T>()),printPrefix<T>())==0) {
            at=std::strlen(printPrefix<T>()); const auto end=text.find('|',at); const auto digits=text.substr(at,end==std::string::npos?text.size()-at:end-at);
            if(digits.empty()||digits.size()>8)throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
            for(char c:digits) { if(c<'0'||c>'9')throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);count=count*10+c-'0'; }
            if(count>kMaximumIndex)throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
            const size_t fields=count+(std::is_same_v<T,SelectionData>?1:0);
            if(fields && end==std::string::npos) throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
            if(!fields && end!=std::string::npos) throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
            at=end==std::string::npos?text.size():end+1;
        } else if constexpr (std::is_same_v<T,SelectionData>) {
            const char* prefix=text.compare(0,std::strlen("Live2DNative1|"),"Live2DNative1|")==0?"Live2DNative1|":"Live2DNative2|";
            if(text.compare(0,std::strlen(prefix),prefix))throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
            legacyOne=std::strcmp(prefix,"Live2DNative1|")==0; count=legacyOne?1:kLegacySlots;at=std::strlen(prefix);
        } else {
            constexpr const char* prefix="Live2DExpression1|";
            if(text.compare(0,std::strlen(prefix),prefix))throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
            count=kLegacySlots;at=std::strlen(prefix);
        }
        const size_t fields=count+(std::is_same_v<T,SelectionData>?1:0);
        if(fields > text.size()-at+1) throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
        T result; auto& paths=pathsOf(result); paths.reserve(count);
        for(size_t i=0;i<fields;++i) {
            const auto separator=text.find('|',at);const bool last=i+1==fields;
            if((last&&separator!=std::string::npos)||(!last&&separator==std::string::npos))throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT);
            auto value=parseHex(text,at,last?text.size():separator);at=last?text.size():separator+1;
            if constexpr(std::is_same_v<T,SelectionData>) {if(i==0)result.model=std::move(value);else paths.push_back(std::move(value));}
            else paths.push_back(std::move(value));
        }
        if(legacyOne)paths.resize(kLegacySlots);
        (void)wireSize(result);return result;
    } catch(...) { throw static_cast<PF_Err>(PF_Err_CANNOT_PARSE_KEYFRAME_TEXT); }
}
template<class T> PF_Err bankCallback(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    const auto read=[&](PF_ArbitraryH h){return readBank<T>(in_data,h);};
    switch(extra->which_function) {
    case PF_Arbitrary_NEW_FUNC:*extra->u.new_func_params.arbPH=newBank<T>(in_data,std::make_shared<const T>());break;
    case PF_Arbitrary_DISPOSE_FUNC:disposeBank<T>(in_data,extra->u.dispose_func_params.arbH);break;
    case PF_Arbitrary_COPY_FUNC:{auto&p=extra->u.copy_func_params;*p.dst_arbPH=newBank<T>(in_data,read(p.src_arbH));break;}
    case PF_Arbitrary_FLAT_SIZE_FUNC:{auto&p=extra->u.flat_size_func_params;*p.flat_data_sizePLu=static_cast<A_u_long>(wireSize(*read(p.arbH)));break;}
    case PF_Arbitrary_FLATTEN_FUNC:{auto&p=extra->u.flatten_func_params;flattenBank(*read(p.arbH),p.flat_dataPV,p.buf_sizeLu);break;}
    case PF_Arbitrary_UNFLATTEN_FUNC:{auto&p=extra->u.unflatten_func_params;*p.arbPH=newBank<T>(in_data,std::make_shared<const T>(unflattenBank<T>(p.flat_dataPV,p.buf_sizeLu)));break;}
    case PF_Arbitrary_INTERP_FUNC:{auto&p=extra->u.interp_func_params;*p.interpPH=newBank<T>(in_data,read(p.tF<1?p.left_arbH:p.right_arbH));break;}
    case PF_Arbitrary_COMPARE_FUNC:{auto&p=extra->u.compare_func_params;const auto a=read(p.a_arbH),b=read(p.b_arbH);int order=0;
        if constexpr(std::is_same_v<T,SelectionData>)order=a->model.compare(b->model);
        if(!order&&pathsOf(*a)!=pathsOf(*b))order=pathsOf(*a)<pathsOf(*b)?-1:1;
        *p.compareP=!order?PF_ArbCompare_EQUAL:order<0?PF_ArbCompare_LESS:PF_ArbCompare_MORE;break;}
    case PF_Arbitrary_PRINT_SIZE_FUNC:{auto&p=extra->u.print_size_func_params;*p.print_sizePLu=static_cast<A_u_long>(printableBank(*read(p.arbH)).size()+1);break;}
    case PF_Arbitrary_PRINT_FUNC:{auto&p=extra->u.print_func_params;const auto text=printableBank(*read(p.arbH));if(!p.print_bufferPC||p.print_sizeLu<text.size()+1)return PF_Err_BAD_CALLBACK_PARAM;std::memcpy(p.print_bufferPC,text.c_str(),text.size()+1);break;}
    case PF_Arbitrary_SCAN_FUNC:{auto&p=extra->u.scan_func_params;*p.arbPH=newBank<T>(in_data,std::make_shared<const T>(scanBank<T>(p.bufPC,p.bytes_to_scanLu)));break;}
    default:return PF_Err_BAD_CALLBACK_PARAM;
    }return PF_Err_NONE;
}
PF_Err arbitraryCallback(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    if(!extra)return PF_Err_BAD_CALLBACK_PARAM;
    if(extra->id==kSelectionDiskId)return bankCallback<SelectionData>(in_data,extra);
    if(extra->id==kExpressionDiskId)return bankCallback<ExpressionData>(in_data,extra);
    return PF_Err_BAD_CALLBACK_PARAM;
}
