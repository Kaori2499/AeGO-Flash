cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(BUILD_AE_PLUGIN "Build the AEX plugin (requires Adobe SDK)" ON)
option(USE_ADOBE_PIPL_TOOL "Regenerate PiPL through the optional Adobe PiPLtool instead of the authored RC resource" OFF)
project(AeGOFlash VERSION 1.0.2 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "AeGO Flash requires Windows x64 and Visual Studio 2022 (v143).")
endif()
if(MSVC_VERSION LESS 1930 OR MSVC_VERSION GREATER_EQUAL 1950)
    message(FATAL_ERROR "Use Visual Studio 2022 with the v143 toolset matching Cubism Core.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(AE_SDK_ROOT "" CACHE PATH "Adobe SDK root containing Examples/Headers")
set(CUBISM_SDK_ROOT "" CACHE PATH "Official CubismSdkForNative-5-r.5 root")
set(L2DAE_TEST_MODEL "" CACHE FILEPATH "Optional .model3.json for native render acceptance test")
set(L2DAE_TEST_MOTION "" CACHE FILEPATH "Optional .motion3.json for native render acceptance test")

foreach(required
    "${CUBISM_SDK_ROOT}/Core/include/Live2DCubismCore.h"
    "${CUBISM_SDK_ROOT}/Core/lib/windows/x86_64/143/Live2DCubismCore_MT.lib"
    "${CUBISM_SDK_ROOT}/Framework/CMakeLists.txt"
    "${CUBISM_SDK_ROOT}/Framework/src/Rendering/D3D11/CubismDeviceInfo_D3D11.hpp")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing SDK file: ${required}. See README.md for the official SDK setup.")
    endif()
endforeach()
if(BUILD_AE_PLUGIN)
enable_language(RC)
foreach(required "${AE_SDK_ROOT}/Examples/Headers/AE_Effect.h" "${AE_SDK_ROOT}/Examples/Headers/AE_EffectCB.h")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing Adobe SDK file: ${required}")
    endif()
endforeach()
find_path(AE_ENTRY_INCLUDE NAMES entry.h
    PATHS "${AE_SDK_ROOT}/Examples/Headers" "${AE_SDK_ROOT}/Examples/Util"
    NO_DEFAULT_PATH REQUIRED)
if(USE_ADOBE_PIPL_TOOL)
    find_program(AE_PIPL_TOOL NAMES PiPLtool.exe PiPLTool.exe
        PATHS "${AE_SDK_ROOT}/Examples/Resources" NO_DEFAULT_PATH REQUIRED)
endif()
endif()

add_library(CubismCore STATIC IMPORTED)
set_target_properties(CubismCore PROPERTIES
    IMPORTED_LOCATION "${CUBISM_SDK_ROOT}/Core/lib/windows/x86_64/143/Live2DCubismCore_MT.lib"
    IMPORTED_LOCATION_DEBUG "${CUBISM_SDK_ROOT}/Core/lib/windows/x86_64/143/Live2DCubismCore_MTd.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CUBISM_SDK_ROOT}/Core/include")
set(FRAMEWORK_SOURCE D3D11)
include(tools/PrepareCubism.cmake)
l2dae_prepare_cubism_framework("${CUBISM_SDK_ROOT}" L2DAE_FRAMEWORK_SOURCE)
add_subdirectory("${L2DAE_FRAMEWORK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/CubismFramework")
target_link_libraries(Framework PUBLIC CubismCore d3d11 d3dcompiler)
target_compile_definitions(Framework PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(Live2DRenderer STATIC src/Renderer.cpp src/MotionJson.cpp)
target_include_directories(Live2DRenderer PUBLIC src)
target_compile_definitions(Live2DRenderer PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(Live2DRenderer PRIVATE /utf-8 /W4 /EHsc)
target_link_libraries(Live2DRenderer PUBLIC Framework dxgi windowscodecs ole32)

add_library(Live2DAudioEnvelope STATIC src/AudioEnvelope.cpp)
target_include_directories(Live2DAudioEnvelope PUBLIC src)
target_compile_options(Live2DAudioEnvelope PRIVATE /utf-8 /W4 /EHsc)

if(BUILD_AE_PLUGIN)
if(USE_ADOBE_PIPL_TOOL)
set(PIPL_RC "${CMAKE_CURRENT_BINARY_DIR}/PluginPiPL.rc")
add_custom_command(OUTPUT "${PIPL_RC}"
    COMMAND "${CMAKE_COMMAND}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/PluginPiPL.r"
        "-DHEADERS=${AE_SDK_ROOT}/Examples/Headers"
        "-DRESOURCES=${AE_SDK_ROOT}/Examples/Resources"
        "-DPIPL_TOOL=${AE_PIPL_TOOL}"
        "-DOUTPUT=${PIPL_RC}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/BuildPiPL.cmake"
    DEPENDS src/PluginPiPL.r "${CMAKE_CURRENT_SOURCE_DIR}/tools/BuildPiPL.cmake"
    VERBATIM)
set_source_files_properties("${PIPL_RC}" PROPERTIES GENERATED TRUE)
else()
set(PIPL_RC "${CMAKE_CURRENT_SOURCE_DIR}/src/PluginPiPL.rc")
endif()
# Public version metadata is independent of the monotonic AE compatibility
# version in PiPL, and is compiled with either PiPL generation path.
add_library(Live2DNativeAE MODULE src/Plugin.cpp src/Dialog.cpp src/TimelineHost.cpp
    src/PluginVersion.rc "${PIPL_RC}")
set_target_properties(Live2DNativeAE PROPERTIES PREFIX "" SUFFIX ".aex" OUTPUT_NAME "AeGOFlash")
# Keep the adjacent helper current even when only the timeline script changes.
set_property(TARGET Live2DNativeAE APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/TimelineClips.jsx")
target_include_directories(Live2DNativeAE PRIVATE
    "${AE_SDK_ROOT}/Examples/Headers"
    "${AE_SDK_ROOT}/Examples/Headers/SP"
    "${AE_ENTRY_INCLUDE}"
    "${AE_SDK_ROOT}/Examples/Util")
target_compile_definitions(Live2DNativeAE PRIVATE _WINDOWS MSWindows)
target_compile_options(Live2DNativeAE PRIVATE /utf-8 /W4 /EHsc)
target_link_libraries(Live2DNativeAE PRIVATE Live2DRenderer Live2DAudioEnvelope comdlg32 user32 gdi32 version)

add_custom_command(TARGET Live2DNativeAE POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CUBISM_SDK_ROOT}/Framework/src/Rendering/D3D11/Shaders"
        "$<TARGET_FILE_DIR:Live2DNativeAE>/FrameworkShaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
        "$<TARGET_FILE_DIR:Live2DNativeAE>/scripts"
    VERBATIM)
endif()

option(BUILD_TESTING "Build the native model, AE adapter and hidden-dialog acceptance tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_executable(Live2DHostProcessTest tests/HostProcess.test.cpp)
    target_include_directories(Live2DHostProcessTest PRIVATE src)
    target_compile_options(Live2DHostProcessTest PRIVATE /utf-8 /W4 /EHsc)
    target_link_libraries(Live2DHostProcessTest PRIVATE version)
    add_test(NAME HostProcessVersion COMMAND Live2DHostProcessTest)
    set_tests_properties(HostProcessVersion PROPERTIES TIMEOUT 30)
    add_executable(Live2DImportPreferencesTest tests/ImportPreferences.test.cpp)
    target_include_directories(Live2DImportPreferencesTest PRIVATE src)
    target_compile_options(Live2DImportPreferencesTest PRIVATE /utf-8 /W4 /EHsc)
    add_test(NAME ImportPreferences COMMAND Live2DImportPreferencesTest "${CMAKE_CURRENT_BINARY_DIR}/preference-test-workspace")
    set_tests_properties(ImportPreferences PROPERTIES TIMEOUT 30)
    add_executable(Live2DHostTextTest tests/HostText.test.cpp)
    target_include_directories(Live2DHostTextTest PRIVATE src)
    target_compile_options(Live2DHostTextTest PRIVATE /utf-8 /W4 /EHsc)
    add_test(NAME HostTextCompatibility COMMAND Live2DHostTextTest)
    set_tests_properties(HostTextCompatibility PROPERTIES TIMEOUT 30)
    add_executable(Live2DOvershootProbe tests/OvershootProbe.cpp)
    target_link_libraries(Live2DOvershootProbe PRIVATE Live2DRenderer)
    target_compile_options(Live2DOvershootProbe PRIVATE /utf-8 /W4 /EHsc)
    add_executable(Live2DUiTextTest tests/UiText.test.cpp)
    target_include_directories(Live2DUiTextTest PRIVATE src)
    target_compile_options(Live2DUiTextTest PRIVATE /utf-8 /W4 /EHsc)
    add_test(NAME ChineseUiMessages COMMAND Live2DUiTextTest)
    set_tests_properties(ChineseUiMessages PROPERTIES TIMEOUT 30)
    add_executable(Live2DMotionJsonSmoke tests/MotionJsonSmoke.cpp src/MotionJson.cpp)
    target_include_directories(Live2DMotionJsonSmoke PRIVATE src)
    target_compile_options(Live2DMotionJsonSmoke PRIVATE /utf-8 /W4 /EHsc)
    add_test(NAME MotionJsonCompatibility COMMAND Live2DMotionJsonSmoke)
    set_tests_properties(MotionJsonCompatibility PROPERTIES TIMEOUT 30)

    # Optional real-model probe; private model assets are never packaged.
    add_executable(Live2DMotionCompatibilityProbe tests/MotionCompatibilityProbe.cpp)
    target_link_libraries(Live2DMotionCompatibilityProbe PRIVATE Live2DRenderer)
    target_compile_options(Live2DMotionCompatibilityProbe PRIVATE /utf-8 /W4 /EHsc)
    add_executable(Live2DAudioEnvelopeTest tests/AudioEnvelope.test.cpp)
    target_compile_options(Live2DAudioEnvelopeTest PRIVATE /utf-8 /W4 /EHsc)
    target_link_libraries(Live2DAudioEnvelopeTest PRIVATE Live2DAudioEnvelope)
    add_test(NAME AudioEnvelope COMMAND Live2DAudioEnvelopeTest)
    set_tests_properties(AudioEnvelope PROPERTIES TIMEOUT 30)
    # DialogSmoke includes the dialog source and supplies its own listMotions
    # fixture, so it does not link Cubism or the renderer and never shows a UI.
    add_executable(Live2DDialogSmoke tests/DialogSmoke.cpp)
    target_compile_definitions(Live2DDialogSmoke PRIVATE UNICODE _UNICODE)
    target_compile_options(Live2DDialogSmoke PRIVATE /utf-8 /W4 /EHsc)
    target_link_libraries(Live2DDialogSmoke PRIVATE comdlg32 user32 gdi32)
    add_test(NAME DialogMotionSearch COMMAND Live2DDialogSmoke)
    set_tests_properties(DialogMotionSearch PROPERTIES TIMEOUT 30)

    # Optional interactive acceptance tool, intentionally not an automated CTest.
    add_executable(Live2DDialogPreview tests/DialogPreview.cpp src/Dialog.cpp)
    target_compile_options(Live2DDialogPreview PRIVATE /utf-8 /W4 /EHsc)
    target_link_libraries(Live2DDialogPreview PRIVATE Live2DRenderer comdlg32 user32 gdi32)

    if(BUILD_AE_PLUGIN)
        # Each executable is a separate cold process with numeric AE host
        # version resources. No discovery entry is called: this exercises the
        # same missing-registration state as a cached plug-in load.
        foreach(host_spec IN ITEMS 22_0 24_0 25_3 25_6 26_3)
            string(REPLACE "_" ";" host_parts "${host_spec}")
            list(GET host_parts 0 host_major)
            list(GET host_parts 1 host_minor)
            set(host_target "Live2DCachedHost_${host_spec}")
            add_executable(${host_target} tests/CachedHostSmoke.cpp tests/CachedHostVersion.rc)
            set_target_properties(${host_target} PROPERTIES OUTPUT_NAME "AfterFX"
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cached-hosts/${host_spec}")
            target_include_directories(${host_target} PRIVATE src
                "${AE_SDK_ROOT}/Examples/Headers" "${AE_SDK_ROOT}/Examples/Headers/SP"
                "${AE_SDK_ROOT}/Examples/Util")
            target_compile_definitions(${host_target} PRIVATE _WINDOWS MSWindows UNICODE _UNICODE
                AE_TEST_HOST_MAJOR=${host_major} AE_TEST_HOST_MINOR=${host_minor})
            target_compile_options(${host_target} PRIVATE /utf-8 /W4 /EHsc)
            target_link_libraries(${host_target} PRIVATE version)
            foreach(first_selector IN ITEMS global params about)
                add_test(NAME CachedHost_${host_spec}_${first_selector}
                    COMMAND ${host_target} "$<TARGET_FILE:Live2DNativeAE>" "${first_selector}")
                set_tests_properties(CachedHost_${host_spec}_${first_selector} PROPERTIES TIMEOUT 30)
            endforeach()
        endforeach()
        # Load the actual AEX and call its exports through official SDK types.
        # The optional model additionally exercises the full native rendering path.
        add_executable(Live2DAdapterSmoke tests/AdapterSmoke.cpp src/TimelineHost.cpp)
        target_include_directories(Live2DAdapterSmoke PRIVATE
            "${AE_SDK_ROOT}/Examples/Util"
            "${AE_SDK_ROOT}/Examples/Headers"
            "${AE_SDK_ROOT}/Examples/Headers/SP")
        target_compile_definitions(Live2DAdapterSmoke PRIVATE _WINDOWS MSWindows UNICODE _UNICODE)
        target_compile_options(Live2DAdapterSmoke PRIVATE /utf-8 /W4 /EHsc)
        target_link_libraries(Live2DAdapterSmoke PRIVATE Live2DAudioEnvelope ole32 user32)
        add_dependencies(Live2DAdapterSmoke Live2DNativeAE)
        if(L2DAE_TEST_MODEL)
            add_test(NAME AdapterCallbacks COMMAND Live2DAdapterSmoke
                "$<TARGET_FILE:Live2DNativeAE>" "${L2DAE_TEST_MODEL}")
        else()
            add_test(NAME AdapterCallbacks COMMAND Live2DAdapterSmoke
                "$<TARGET_FILE:Live2DNativeAE>")
        endif()
        set_tests_properties(AdapterCallbacks PROPERTIES TIMEOUT 120)

        # Retain the host's outer parameter locks across a successful import.
        # This fixture includes Plugin.cpp and supplies the UI/renderer/host stubs.
        add_executable(Live2DImportCommitSmoke tests/ImportCommitSmoke.cpp)
        target_include_directories(Live2DImportCommitSmoke PRIVATE
            "${AE_SDK_ROOT}/Examples/Headers"
            "${AE_SDK_ROOT}/Examples/Headers/SP"
            "${AE_SDK_ROOT}/Examples/Util")
        target_compile_definitions(Live2DImportCommitSmoke PRIVATE _WINDOWS MSWindows UNICODE _UNICODE)
        target_compile_options(Live2DImportCommitSmoke PRIVATE /utf-8 /W4 /EHsc)
        # Match AfterFX.exe's 8 MiB host stack for this in-process fixture.
        target_link_options(Live2DImportCommitSmoke PRIVATE /STACK:8388608)
        target_link_libraries(Live2DImportCommitSmoke PRIVATE Live2DAudioEnvelope version)
        add_test(NAME ImportCommitOwnership COMMAND Live2DImportCommitSmoke)
        set_tests_properties(ImportCommitOwnership PROPERTIES TIMEOUT 30)

        add_executable(Live2DTimelineGuardSmoke tests/TimelineGuardSmoke.cpp src/TimelineHost.cpp)
        target_include_directories(Live2DTimelineGuardSmoke PRIVATE
            "${AE_SDK_ROOT}/Examples/Headers"
            "${AE_SDK_ROOT}/Examples/Headers/SP")
        target_compile_definitions(Live2DTimelineGuardSmoke PRIVATE _WINDOWS MSWindows UNICODE _UNICODE)
        target_compile_options(Live2DTimelineGuardSmoke PRIVATE /utf-8 /W4 /EHsc)
        target_link_libraries(Live2DTimelineGuardSmoke PRIVATE ole32 user32)
        add_test(NAME TimelineImportGuard COMMAND Live2DTimelineGuardSmoke)
        set_tests_properties(TimelineImportGuard PROPERTIES TIMEOUT 30)
    endif()

    add_executable(Live2DRenderProbe tests/RenderProbe.cpp)
    add_executable(Live2DTransitionProbe tests/TransitionProbe.cpp)
    target_link_libraries(Live2DTransitionProbe PRIVATE Live2DRenderer)
    target_compile_options(Live2DTransitionProbe PRIVATE /utf-8 /W4 /EHsc)
    add_executable(Live2DOcclusionTransitionProbe tests/OcclusionTransitionProbe.cpp)
    target_link_libraries(Live2DOcclusionTransitionProbe PRIVATE Live2DRenderer)
    target_compile_options(Live2DOcclusionTransitionProbe PRIVATE /utf-8 /W4 /EHsc)
    target_link_libraries(Live2DRenderProbe PRIVATE Live2DRenderer)
    target_compile_options(Live2DRenderProbe PRIVATE /utf-8 /W4 /EHsc)
    add_executable(Live2DRenderConcurrency tests/RenderConcurrency.cpp)
    target_link_libraries(Live2DRenderConcurrency PRIVATE Live2DRenderer psapi)
    target_compile_definitions(Live2DRenderConcurrency PRIVATE L2DAE_MFR_DIAGNOSTICS=1)
    target_compile_options(Live2DRenderConcurrency PRIVATE /utf-8 /W4 /EHsc)
    add_executable(Live2DPhysicsResetTest tests/PhysicsReset.test.cpp)
    target_link_libraries(Live2DPhysicsResetTest PRIVATE Framework)
    target_compile_options(Live2DPhysicsResetTest PRIVATE /utf-8 /W4 /EHsc)
    add_executable(Live2DCoreResetTest tests/CoreReset.test.cpp)
    target_link_libraries(Live2DCoreResetTest PRIVATE Framework)
    target_compile_options(Live2DCoreResetTest PRIVATE /utf-8 /W4 /EHsc)
    add_executable(Live2DRenderOrderOverrideTest tests/RenderOrderOverride.test.cpp)
    target_link_libraries(Live2DRenderOrderOverrideTest PRIVATE Framework)
    target_compile_options(Live2DRenderOrderOverrideTest PRIVATE /utf-8 /W4 /EHsc)
    if(EXISTS "${CUBISM_SDK_ROOT}/Samples/Resources")
        add_test(NAME PhysicsResetState COMMAND Live2DPhysicsResetTest "${CUBISM_SDK_ROOT}/Samples/Resources")
        set_tests_properties(PhysicsResetState PROPERTIES TIMEOUT 30)
        add_test(NAME CoreResetState COMMAND Live2DCoreResetTest "${CUBISM_SDK_ROOT}/Samples/Resources")
        set_tests_properties(CoreResetState PROPERTIES TIMEOUT 60)
        add_test(NAME RenderOrderOverride COMMAND Live2DRenderOrderOverrideTest "${CUBISM_SDK_ROOT}/Samples/Resources/Haru/Haru.model3.json")
        set_tests_properties(RenderOrderOverride PROPERTIES TIMEOUT 30)
    endif()
    add_custom_command(TARGET Live2DRenderConcurrency POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CUBISM_SDK_ROOT}/Framework/src/Rendering/D3D11/Shaders"
            "$<TARGET_FILE_DIR:Live2DRenderConcurrency>/FrameworkShaders"
        VERBATIM)
    add_custom_command(TARGET Live2DRenderProbe POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CUBISM_SDK_ROOT}/Framework/src/Rendering/D3D11/Shaders"
            "$<TARGET_FILE_DIR:Live2DRenderProbe>/FrameworkShaders"
        VERBATIM)
    if(L2DAE_TEST_MODEL)
        add_test(NAME NativeRenderConcurrency COMMAND Live2DRenderConcurrency "${L2DAE_TEST_MODEL}")
        set_tests_properties(NativeRenderConcurrency PROPERTIES TIMEOUT 180)
        if(L2DAE_TEST_MOTION)
            add_test(NAME NativeRenderSeek COMMAND Live2DRenderProbe "${L2DAE_TEST_MODEL}" "${L2DAE_TEST_MOTION}")
        else()
            add_test(NAME NativeRenderSeek COMMAND Live2DRenderProbe "${L2DAE_TEST_MODEL}")
        endif()
    endif()
endif()

if(BUILD_AE_PLUGIN)
install(TARGETS Live2DNativeAE LIBRARY DESTINATION AeGOFlash RUNTIME DESTINATION AeGOFlash)
install(DIRECTORY "${CUBISM_SDK_ROOT}/Framework/src/Rendering/D3D11/Shaders/"
    DESTINATION AeGOFlash/FrameworkShaders)
install(FILES README.md THIRD_PARTY.md 安装说明.txt DESTINATION AeGOFlash)
install(DIRECTORY scripts/ DESTINATION AeGOFlash/scripts FILES_MATCHING PATTERN "*.jsx")
foreach(license "${CUBISM_SDK_ROOT}/Core/LICENSE.md" "${CUBISM_SDK_ROOT}/Framework/LICENSE.md")
    if(EXISTS "${license}")
        get_filename_component(license_parent "${license}" DIRECTORY)
        get_filename_component(license_kind "${license_parent}" NAME)
        install(FILES "${license}" DESTINATION AeGOFlash/licenses RENAME "${license_kind}-LICENSE.md")
    endif()
endforeach()
endif()
