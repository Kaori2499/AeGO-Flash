# Build-only extension for the official CubismSdkForNative-5-r.5 Framework.
# The installed/user-supplied SDK is never edited. All consumers must compile
# against the returned Framework source directory, including its public header.
# Usage:
#   include(tools/PrepareCubism.cmake)
#   l2dae_prepare_cubism_framework("${CUBISM_SDK_ROOT}" patched_framework)
#   add_subdirectory("${patched_framework}" "${CMAKE_CURRENT_BINARY_DIR}/CubismFramework")
#
# Audit basis: official Native 5-r.5 download; SHA-256 checks below deliberately
# reject changed versions and local changes to the reset/allocation semantics.
# ResetForEvaluation restores the successful Create() state without re-parsing
# JSON or entering the global ID manager. The caller still restores the model's
# baseline, applies its first motion sample, then calls Stabilization as before.
# Each physics object belongs exclusively to one mutable model slot.
#
# Mutable-state inventory:
# - constructor Options and fractional clock;
# - both parameter caches (empty logical size, retained capacity);
# - current/previous output values, cached input/output indices, output extrema;
# - every particle field, including the JSON root Position omitted by Reset();
# - rig Gravity/Wind, retained exactly as they were after successful Create().
# JSON configuration, FPS, IDs and function pointers are immutable after parse.
# Typed particle copies use the normal C++ value semantics, never object bytes.
# Existing Reset(), stabilization, integration and interpolation are untouched.
#
# ResetModelForEvaluation uses Core's public csmInitializeModelInPlace API on
# the existing, correctly aligned allocation. Core's private incremental
# deformation state is recreated without copying opaque bytes, reallocating
# models, recreating textures, or changing the read-only shared MOC. The model
# records its owning MOC/allocation size at creation. Layout/count invariants
# protect Framework/renderer caches; only the four public-array cache pointers
# are refreshed. Do not call CubismModel::Initialize() again: it appends IDs.
# Call while the worker owns the model exclusively and under the SDK mutex,
# then restore all real/virtual parameters, parts, pose and physics as usual.
# Render-order overrides live only in Framework-owned reusable vectors. Input
# must be a complete permutation of drawable+offscreen ranks. Invalid input
# preserves the previous valid override; clearing and Core reset disable it.
# The Core-owned const render-order array is never written.

include_guard(GLOBAL)

function(_l2dae_cubism_require_sha path expected)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Cubism Native 5-r.5 audit input is missing: ${path}")
    endif()
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "Unsupported Cubism Framework input: ${path}\n"
            "Expected the unmodified official Native 5-r.5 file (${expected}); got ${actual}.\n"
            "ResetForEvaluation must be re-audited before another SDK revision is used.")
    endif()
endfunction()

function(_l2dae_cubism_replace_once variable needle replacement)
    set(value "${${variable}}")
    string(FIND "${value}" "${needle}" first)
    if(first LESS 0)
        message(FATAL_ERROR "Cubism 5-r.5 patch anchor is missing: ${needle}")
    endif()
    string(LENGTH "${needle}" length)
    math(EXPR remainder_start "${first} + ${length}")
    string(SUBSTRING "${value}" ${remainder_start} -1 remainder)
    string(FIND "${remainder}" "${needle}" duplicate)
    if(NOT duplicate EQUAL -1)
        message(FATAL_ERROR "Cubism 5-r.5 patch anchor is ambiguous: ${needle}")
    endif()
    string(REPLACE "${needle}" "${replacement}" value "${value}")
    set(${variable} "${value}" PARENT_SCOPE)
endfunction()

function(_l2dae_cubism_write_if_changed path contents)
    if(EXISTS "${path}")
        file(READ "${path}" existing)
        if(existing STREQUAL contents)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${contents}")
endfunction()

function(l2dae_prepare_cubism_framework sdk_root out_var)
    file(REAL_PATH "${sdk_root}" source_sdk)
    set(source "${source_sdk}/Framework")
    set(destination "${CMAKE_CURRENT_BINARY_DIR}/cubism-native-5-r.5-patched/Framework")
    file(REAL_PATH "${destination}" resolved_destination)
    # Prevent an in-SDK build directory or a pre-existing junction from turning
    # the build-only copy into a modification of the caller's official SDK.
    string(TOLOWER "${source_sdk}" sdk_lower)
    string(TOLOWER "${resolved_destination}" destination_lower)
    cmake_path(IS_PREFIX sdk_lower "${destination_lower}" NORMALIZE inside_sdk)
    if(inside_sdk)
        message(FATAL_ERROR "The patched Framework must be generated outside the official SDK: ${destination}")
    endif()

    _l2dae_cubism_require_sha("${source}/src/Physics/CubismPhysics.cpp"
        "b967e6559f1b1f96b36148bd909e43d515ed6cdf4398fdd0c1250b3cdd1c4623")
    _l2dae_cubism_require_sha("${source}/src/Physics/CubismPhysics.hpp"
        "b171ba5612d2c2a9e00ec2c8219237519183d5b643e602f4fd15c83943402b67")
    _l2dae_cubism_require_sha("${source}/src/Physics/CubismPhysicsInternal.hpp"
        "7ac82e50e5d5f7720fe10deb3bf701f691f52d3553b4e90f2304d47f95785b57")
    _l2dae_cubism_require_sha("${source}/src/Type/csmVector.hpp"
        "5ca108f6cb1612ff6a5b8bfeb3d31b9ccf63460e98ef0909ca58ef1e83eb90be")
    _l2dae_cubism_require_sha("${source}/src/CubismFrameworkConfig.hpp"
        "a9251edcf712050e8cfe88c7c21c65d4f2c39416193fa2f970074399c9a69db9")
    _l2dae_cubism_require_sha("${source}/src/CubismFramework.cpp"
        "2e4191bcbc84e8b3fc0836930d6524d0a8981e8c58f6383100e7713bc517327c")
    _l2dae_cubism_require_sha("${source}/src/Model/CubismMoc.hpp"
        "bf74e65f92814642889b42941133e9d5f33ee44adcfe843fc6554bfd246d7ac1")
    _l2dae_cubism_require_sha("${source}/src/Model/CubismMoc.cpp"
        "739c0e9c10c40503b28bdb71908195fe3079510a5738649f399e61c117c4dcca")
    _l2dae_cubism_require_sha("${source}/src/Model/CubismModel.hpp"
        "b93e82a630278624d2ac2745c275eceb31d54c54212356c38fdd8f39f619337e")
    _l2dae_cubism_require_sha("${source}/src/Model/CubismModel.cpp"
        "b0da23c63a8c10bf90a7f68cb7779adc725f41d0241a1093ea3fc214d9b56ffb")
    _l2dae_cubism_require_sha("${source_sdk}/Core/include/Live2DCubismCore.h"
        "6f1802780d1eb36ff39705e0764f9eeed9b41c313a13ac155270c6f4ad51d53f")
    _l2dae_cubism_require_sha("${source_sdk}/Core/lib/windows/x86_64/143/Live2DCubismCore_MT.lib"
        "177c0c58b5d8ee5992d578d9ac19decad04e2fb87a0a5a4ad4bcb6d601e52d56")

    # Derive each patch from the original inputs, making repeated configuration
    # idempotent even when the existing destination already contains the patch.
    file(READ "${source}/src/Physics/CubismPhysics.hpp" header)
    file(READ "${source}/src/Physics/CubismPhysics.cpp" implementation)
    string(REPLACE "\r\n" "\n" header "${header}")
    string(REPLACE "\r\n" "\n" implementation "${implementation}")

    _l2dae_cubism_replace_once(header "    void Reset();" [=[    void Reset();

    // Live2DNativeAE build-only extension, audited for Native 5-r.5.
    // Restore fresh Create() state without JSON parsing. Exclusive slot access
    // is required; restore model values and call Stabilization before stepping.
    void ResetForEvaluation();]=])

    _l2dae_cubism_replace_once(header "    CubismPhysicsRig* _physicsRig;" [=[    // Immutable typed snapshot captured after successful Create().
    csmVector<CubismPhysicsParticle> _initialEvaluationParticles;
    CubismVector2 _initialEvaluationGravity;
    CubismVector2 _initialEvaluationWind;

    CubismPhysicsRig* _physicsRig;]=])

    _l2dae_cubism_replace_once(implementation
        "    ret->_physicsRig->Gravity.Y = 0;\n    return ret;" [=[    ret->_physicsRig->Gravity.Y = 0;
    // Capture once, including the root Position that Initialize() leaves intact.
    ret->_initialEvaluationParticles = ret->_physicsRig->Particles;
    ret->_initialEvaluationGravity = ret->_physicsRig->Gravity;
    ret->_initialEvaluationWind = ret->_physicsRig->Wind;
    return ret;]=])

    set(reset_definition [=[// Live2DNativeAE build-only extension. Keep official Reset() semantics intact.
void CubismPhysics::ResetForEvaluation()
{
    _options.Gravity.X = 0.0f;
    _options.Gravity.Y = -1.0f;
    _options.Wind.X = 0.0f;
    _options.Wind.Y = 0.0f;
    _currentRemainTime = 0.0f;
    _physicsRig->Gravity = _initialEvaluationGravity;
    _physicsRig->Wind = _initialEvaluationWind;

    // Resize(0) keeps capacity in the pinned R5 csmVector. Stabilization will
    // recreate/overwrite all logical entries without an allocation after warmup.
    _parameterCaches.Resize(0);
    _parameterInputCaches.Resize(0);

    for (csmUint32 i = 0; i < _physicsRig->Particles.GetSize(); ++i)
    {
        _physicsRig->Particles[i] = _initialEvaluationParticles[i];
    }
    for (csmUint32 i = 0; i < _physicsRig->Inputs.GetSize(); ++i)
    {
        _physicsRig->Inputs[i].SourceParameterIndex = -1;
    }
    for (csmUint32 i = 0; i < _physicsRig->Outputs.GetSize(); ++i)
    {
        _physicsRig->Outputs[i].DestinationParameterIndex = -1;
        _physicsRig->Outputs[i].ValueBelowMinimum = 0.0f;
        _physicsRig->Outputs[i].ValueExceededMaximum = 0.0f;
    }
    for (csmUint32 i = 0; i < _currentRigOutputs.GetSize(); ++i)
    {
        for (csmUint32 j = 0; j < _currentRigOutputs[i].outputs.GetSize(); ++j)
        {
            _currentRigOutputs[i].outputs[j] = 0.0f;
            _previousRigOutputs[i].outputs[j] = 0.0f;
        }
    }
}

]=])
    set(create_signature "CubismPhysics* CubismPhysics::Create(const csmByte* buffer, csmSizeInt size)")
    _l2dae_cubism_replace_once(implementation "${create_signature}" "${reset_definition}${create_signature}")
    _l2dae_cubism_replace_once(implementation
        "#include \"Math/CubismVector2.hpp\"" [=[#include "Math/CubismVector2.hpp"

// Framework's optional allocation tracker writes an unsynchronized global
// vector. It cannot be enabled for parallel per-model CPU evaluation.
#if defined(CSM_DEBUG_MEMORY_LEAKING)
#error Live2DNativeAE parallel rendering requires CSM_DEBUG_MEMORY_LEAKING disabled.
#endif]=])

    file(READ "${source}/src/Model/CubismMoc.hpp" moc_header)
    file(READ "${source}/src/Model/CubismMoc.cpp" moc_implementation)
    file(READ "${source}/src/Model/CubismModel.hpp" model_header)
    file(READ "${source}/src/Model/CubismModel.cpp" model_implementation)
    foreach(variable moc_header moc_implementation model_header model_implementation)
        string(REPLACE "\r\n" "\n" ${variable} "${${variable}}")
    endforeach()
    _l2dae_cubism_replace_once(moc_header "    CubismModel* CreateModel();" [=[    CubismModel* CreateModel();

    // Live2DNativeAE build-only extension for the pinned Core/Framework.
    // Reinitialize Core in its existing allocation. Exclusive model ownership
    // and the caller's SDK mutex are required. False means the caller must
    // discard the model; never continue rendering it after a failed reset.
    csmBool ResetModelForEvaluation(CubismModel* model);]=])
    _l2dae_cubism_replace_once(model_header "    Core::csmModel*     _model;" [=[    Core::csmModel*     _model;

    // Ownership/size guard for CubismMoc::ResetModelForEvaluation.
    const CubismMoc* _evaluationOwnerMoc;
    csmUint32 _evaluationModelBytes;
    csmBool _hasEvaluationRenderOrderOverride;
    csmVector<csmInt32> _evaluationRenderOrders;
    csmVector<csmUint8> _evaluationRenderOrderSeen;]=])
    _l2dae_cubism_replace_once(model_implementation "    : _model(model)" [=[    : _model(model)
    , _evaluationOwnerMoc(NULL)
    , _evaluationModelBytes(0)
    , _hasEvaluationRenderOrderOverride(false)]=])
    _l2dae_cubism_replace_once(model_header "    const csmInt32* GetRenderOrders() const;" [=[    const csmInt32* GetRenderOrders() const;

    // Live2DNativeAE build-only extension. The caller owns this model
    // exclusively. nullptr/0 clears; otherwise input is copied after complete
    // permutation validation. Invalid input leaves the last valid state intact.
    csmBool SetRenderOrderOverrideForEvaluation(const csmInt32* orders, csmInt32 count);]=])
    _l2dae_cubism_replace_once(model_implementation
        "#include \"Math/CubismMath.hpp\"" "#include \"Math/CubismMath.hpp\"\n#include <limits>")
    _l2dae_cubism_replace_once(model_implementation [=[const csmInt32* CubismModel::GetRenderOrders() const
{
    const csmInt32* renderOrders = Core::csmGetRenderOrders(_model);
    return renderOrders;
}]=] [=[const csmInt32* CubismModel::GetRenderOrders() const
{
    const csmInt32 drawableCount = Core::csmGetDrawableCount(_model);
    const csmInt32 offscreenCount = Core::csmGetOffscreenCount(_model);
    if (_hasEvaluationRenderOrderOverride && drawableCount >= 0 && offscreenCount >= 0 &&
        drawableCount <= std::numeric_limits<csmInt32>::max() - offscreenCount)
    {
        const csmInt32 count = drawableCount + offscreenCount;
        if (count > 0 && _evaluationRenderOrders.GetSize() == static_cast<csmUint32>(count))
        {
            return &_evaluationRenderOrders[0];
        }
    }
    return Core::csmGetRenderOrders(_model);
}

csmBool CubismModel::SetRenderOrderOverrideForEvaluation(const csmInt32* orders, csmInt32 count)
{
    if (!orders && count == 0)
    {
        _hasEvaluationRenderOrderOverride = false;
        return true;
    }
    const csmInt32 drawableCount = Core::csmGetDrawableCount(_model);
    const csmInt32 offscreenCount = Core::csmGetOffscreenCount(_model);
    if (!orders || count <= 0 || drawableCount < 0 || offscreenCount < 0 ||
        drawableCount > std::numeric_limits<csmInt32>::max() - offscreenCount ||
        count != drawableCount + offscreenCount)
    {
        return false;
    }
    // Scratch and output retain capacity across both draws and successive
    // frames. Never enable or modify output until the entire input is valid.
    _evaluationRenderOrderSeen.Resize(count, 0);
    for (csmInt32 i = 0; i < count; ++i) _evaluationRenderOrderSeen[i] = 0;
    for (csmInt32 i = 0; i < count; ++i)
    {
        const csmInt32 rank = orders[i];
        if (rank < 0 || rank >= count || _evaluationRenderOrderSeen[rank]) return false;
        _evaluationRenderOrderSeen[rank] = 1;
    }
    _evaluationRenderOrders.Resize(count, 0);
    for (csmInt32 i = 0; i < count; ++i) _evaluationRenderOrders[i] = orders[i];
    _hasEvaluationRenderOrderOverride = true;
    return true;
}]=])
    _l2dae_cubism_replace_once(moc_implementation
        "#include \"CubismModel.hpp\"" "#include \"CubismModel.hpp\"\n#include <cstdint>")
    _l2dae_cubism_replace_once(moc_implementation
        "        cubismModel = CSM_NEW CubismModel(model);\n        cubismModel->Initialize();" [=[        cubismModel = CSM_NEW CubismModel(model);
        cubismModel->_evaluationOwnerMoc = this;
        cubismModel->_evaluationModelBytes = modelSize;
        cubismModel->Initialize();]=])
    set(core_reset_definition [=[// Live2DNativeAE build-only extension. Hash addresses/counts, not opaque
// model bytes. The static pointer tables are also checked entry by entry so a
// reinitialization cannot silently invalidate the renderer's clipping caches.
namespace {
csmUint64 EvaluationLayoutSignature(Core::csmModel* core)
{
    csmUint64 signature = 1469598103934665603ULL;
    const auto number = [&](csmUint64 value) {
        signature ^= value;
        signature *= 1099511628211ULL;
    };
    const auto pointer = [&](const void* value) {
        number(static_cast<csmUint64>(reinterpret_cast<std::uintptr_t>(value)));
    };
    pointer(Core::csmGetParameterValues(core));
    pointer(Core::csmGetParameterMinimumValues(core));
    pointer(Core::csmGetParameterMaximumValues(core));
    pointer(Core::csmGetParameterDefaultValues(core));
    pointer(Core::csmGetParameterTypes(core));
    pointer(Core::csmGetParameterRepeats(core));
    pointer(Core::csmGetPartOpacities(core));
    pointer(Core::csmGetPartParentPartIndices(core));
    pointer(Core::csmGetPartOffscreenIndices(core));
    pointer(Core::csmGetDrawableConstantFlags(core));
    pointer(Core::csmGetDrawableBlendModes(core));
    pointer(Core::csmGetDrawableTextureIndices(core));
    pointer(Core::csmGetDrawableParentPartIndices(core));
    const auto parameterIds = Core::csmGetParameterIds(core);
    const auto partIds = Core::csmGetPartIds(core);
    const auto drawableIds = Core::csmGetDrawableIds(core);
    pointer(parameterIds); pointer(partIds); pointer(drawableIds);
    for (csmInt32 i = 0; i < Core::csmGetParameterCount(core); ++i) pointer(parameterIds[i]);
    for (csmInt32 i = 0; i < Core::csmGetPartCount(core); ++i) pointer(partIds[i]);
    const auto vertices = Core::csmGetDrawableVertexPositions(core);
    const auto uvs = Core::csmGetDrawableVertexUvs(core);
    const auto indices = Core::csmGetDrawableIndices(core);
    const auto masks = Core::csmGetDrawableMasks(core);
    const auto vertexCounts = Core::csmGetDrawableVertexCounts(core);
    const auto indexCounts = Core::csmGetDrawableIndexCounts(core);
    const auto maskCounts = Core::csmGetDrawableMaskCounts(core);
    pointer(vertices); pointer(uvs); pointer(indices); pointer(masks);
    pointer(vertexCounts); pointer(indexCounts); pointer(maskCounts);
    for (csmInt32 i = 0; i < Core::csmGetDrawableCount(core); ++i)
    {
        pointer(drawableIds[i]); pointer(vertices[i]); pointer(uvs[i]);
        pointer(indices[i]); pointer(masks[i]);
        number(vertexCounts[i]); number(indexCounts[i]); number(maskCounts[i]);
    }
    const auto offscreenMasks = Core::csmGetOffscreenMasks(core);
    const auto offscreenMaskCounts = Core::csmGetOffscreenMaskCounts(core);
    pointer(offscreenMasks); pointer(offscreenMaskCounts);
    pointer(Core::csmGetOffscreenBlendModes(core));
    pointer(Core::csmGetOffscreenOwnerIndices(core));
    pointer(Core::csmGetOffscreenConstantFlags(core));
    for (csmInt32 i = 0; i < Core::csmGetOffscreenCount(core); ++i)
    {
        pointer(offscreenMasks[i]); number(offscreenMaskCounts[i]);
    }
    return signature;
}
}

csmBool CubismMoc::ResetModelForEvaluation(CubismModel* model)
{
    if (!model || !model->_model || model->_evaluationOwnerMoc != this)
    {
        return false;
    }
    const csmUint32 modelSize = Core::csmGetSizeofModel(_moc);
    if (!modelSize || modelSize != model->_evaluationModelBytes)
    {
        return false;
    }
    Core::csmModel* const address = model->_model;
    const csmInt32 parameterCount = Core::csmGetParameterCount(address);
    const csmInt32 partCount = Core::csmGetPartCount(address);
    const csmInt32 drawableCount = Core::csmGetDrawableCount(address);
    const csmInt32 offscreenCount = Core::csmGetOffscreenCount(address);
    const csmUint64 layout = EvaluationLayoutSignature(address);
    model->_hasEvaluationRenderOrderOverride = false;
    Core::csmModel* const reinitialized = Core::csmInitializeModelInPlace(_moc, address, modelSize);
    if (!reinitialized || reinitialized != address)
    {
        return false;
    }
    if (Core::csmGetParameterCount(address) != parameterCount ||
        Core::csmGetPartCount(address) != partCount ||
        Core::csmGetDrawableCount(address) != drawableCount ||
        Core::csmGetOffscreenCount(address) != offscreenCount ||
        EvaluationLayoutSignature(address) != layout)
    {
        return false;
    }
    // Only these four Framework fields cache Core-owned array addresses.
    // All ID/virtual-parameter/override vectors remain valid and owned by the
    // same wrapper. The caller restores its saved real/virtual values next.
    model->_parameterValues = Core::csmGetParameterValues(address);
    model->_partOpacities = Core::csmGetPartOpacities(address);
    model->_parameterMaximumValues = Core::csmGetParameterMaximumValues(address);
    model->_parameterMinimumValues = Core::csmGetParameterMinimumValues(address);
    return true;
}

]=])
    _l2dae_cubism_replace_once(moc_implementation "void CubismMoc::DeleteModel(CubismModel* model)"
        "${core_reset_definition}void CubismMoc::DeleteModel(CubismModel* model)")

    file(MAKE_DIRECTORY "${destination}")
    file(COPY "${source}/" DESTINATION "${destination}"
        PATTERN ".git" EXCLUDE
        PATTERN "CubismPhysics.hpp" EXCLUDE
        PATTERN "CubismPhysics.cpp" EXCLUDE
        PATTERN "CubismMoc.hpp" EXCLUDE
        PATTERN "CubismMoc.cpp" EXCLUDE
        PATTERN "CubismModel.hpp" EXCLUDE
        PATTERN "CubismModel.cpp" EXCLUDE)
    _l2dae_cubism_write_if_changed("${destination}/src/Physics/CubismPhysics.hpp" "${header}")
    _l2dae_cubism_write_if_changed("${destination}/src/Physics/CubismPhysics.cpp" "${implementation}")
    _l2dae_cubism_write_if_changed("${destination}/src/Model/CubismMoc.hpp" "${moc_header}")
    _l2dae_cubism_write_if_changed("${destination}/src/Model/CubismMoc.cpp" "${moc_implementation}")
    _l2dae_cubism_write_if_changed("${destination}/src/Model/CubismModel.hpp" "${model_header}")
    _l2dae_cubism_write_if_changed("${destination}/src/Model/CubismModel.cpp" "${model_implementation}")
    set(${out_var} "${destination}" PARENT_SCOPE)
endfunction()
