#include <LuxEditor/Project/ProjectTemplates.hpp>

#include <fstream>

namespace LuxEditor {
namespace {
constexpr const char* GameCMake = R"LUX(cmake_minimum_required(VERSION 3.24)
project(LuxGame LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(LuxEngine CONFIG REQUIRED)
include(${CMAKE_CURRENT_SOURCE_DIR}/Dependencies.cmake OPTIONAL)
add_executable(LuxGame Source/Main.cpp)
find_program(LUX_GAME_DOTNET dotnet HINTS "$ENV{DOTNET_ROOT}" REQUIRED)
set(_lux_managed_output "${CMAKE_CURRENT_SOURCE_DIR}/.lux/managed")
add_custom_target(LuxGameScripts
    COMMAND "${LUX_GAME_DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/GameScripts.csproj"
        --configuration Release --output "${_lux_managed_output}"
        --artifacts-path "${CMAKE_CURRENT_BINARY_DIR}/managed-artifacts"
        "-p:LuxEngineScriptCore=${LuxEngine_MANAGED_DIR}/LuxEngine.ScriptCore.dll"
        "-p:RestoreConfigFile=${CMAKE_CURRENT_SOURCE_DIR}/Scripts/NuGet.Config"
        -p:NuGetAudit=false
    VERBATIM)
add_dependencies(LuxGame LuxGameScripts)
target_link_libraries(LuxGame PRIVATE LuxEngine::Engine LuxEngine::Runtime)
target_compile_definitions(LuxGame PRIVATE LUX_GAME_TEMPLATE_KIND=@KIND@)
if(MSVC)
    # Sources are UTF-8 (Korean text); MSVC would otherwise read them in the system code page.
    target_compile_options(LuxGame PRIVATE /utf-8)
endif()
if(COMMAND lux_copy_runtime)
    lux_copy_runtime(LuxGame NO_PROJECT_CONTENT)
elseif(COMMAND lux_copy_shaders)
    lux_copy_shaders(LuxGame)
endif()
set(_lux_manifest "${CMAKE_CURRENT_SOURCE_DIR}/project.luxproject")
if(NOT EXISTS "${_lux_manifest}")
    set(_lux_manifest "${CMAKE_CURRENT_SOURCE_DIR}/project.luxproj")
endif()
add_custom_target(LuxGameContent
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
        "-DPROJECT_MANIFEST=${_lux_manifest}"
        "-DCONTENT_ROOT=$<TARGET_FILE_DIR:LuxGame>/Content"
        "-DMANAGED_ROOT=${_lux_managed_output}"
        "-DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/lux-content-$<CONFIG>.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/SyncContent.cmake"
    VERBATIM)
add_dependencies(LuxGameContent LuxGameScripts)
add_dependencies(LuxGame LuxGameContent)
if(WIN32)
    get_target_property(_engine_type LuxEngine::Engine TYPE)
    if(_engine_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET LuxGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LuxEngine::Engine> $<TARGET_FILE_DIR:LuxGame>)
    endif()
    if(TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET LuxGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:LuxGame>)
    endif()

endif()
)LUX";

constexpr const char* SyncContent = R"LUX(cmake_minimum_required(VERSION 3.24)
file(GLOB_RECURSE assets LIST_DIRECTORIES false RELATIVE "${SOURCE_ROOT}"
    "${SOURCE_ROOT}/Assets/*"
    "${SOURCE_ROOT}/*.lux" "${SOURCE_ROOT}/*.[lL][uU][xX][pP][rR][eE][fF][aA][bB]"
    "${SOURCE_ROOT}/*.[lL][uU][xX][aA][nN][iI][mM]"
    "${SOURCE_ROOT}/*.[lL][uU][xX][aA][nN][iI][mM][aA][tT][oO][rR]"
    "${SOURCE_ROOT}/*.[oO][bB][jJ]" "${SOURCE_ROOT}/*.[fF][bB][xX]"
    "${SOURCE_ROOT}/*.[pP][nN][gG]" "${SOURCE_ROOT}/*.[jJ][pP][gG]"
    "${SOURCE_ROOT}/*.[jJ][pP][eE][gG]" "${SOURCE_ROOT}/*.[bB][mM][pP]"
    "${SOURCE_ROOT}/*.[tT][gG][aA]" "${SOURCE_ROOT}/*.[hH][dD][rR]")
file(GLOB_RECURSE scripts LIST_DIRECTORIES false RELATIVE "${MANAGED_ROOT}" "${MANAGED_ROOT}/*")
set(projectAssets)
foreach(path IN LISTS assets)
    set(source "${SOURCE_ROOT}/${path}")
    if(DEFINED BINARY_ROOT AND NOT BINARY_ROOT STREQUAL SOURCE_ROOT)
        cmake_path(IS_PREFIX BINARY_ROOT "${source}" NORMALIZE generated)
        if(generated)
            continue()
        endif()
    endif()
    if(path MATCHES "^Assets/" OR
       (NOT path MATCHES "(^|/)([.]git|[.]lux)(/|$)" AND
        NOT path MATCHES "^(build|out|cmake-build[^/]*)(/|$)"))
        list(APPEND projectAssets "${path}")
    endif()
endforeach()
set(current project.luxproject ${projectAssets})
foreach(path IN LISTS scripts)
    list(APPEND current "Scripts/${path}")
endforeach()
file(LOCK "${MANIFEST}.lock" GUARD PROCESS TIMEOUT 30)
if(IS_SYMLINK "${CONTENT_ROOT}")
    message(FATAL_ERROR "Generated content root is a link: ${CONTENT_ROOT}")
endif()
file(MAKE_DIRECTORY "${CONTENT_ROOT}")
file(REAL_PATH "${CONTENT_ROOT}" root)
set(outputLock "${root}.lux-sync-lock.dir")
if(IS_SYMLINK "${outputLock}" OR IS_SYMLINK "${outputLock}/cmake.lock")
    message(FATAL_ERROR "Generated content lock is a link: ${outputLock}")
endif()
file(LOCK "${outputLock}" DIRECTORY GUARD PROCESS TIMEOUT 30)
set(transaction "${root}.lux-sync.dir")
set(nextManifest "${MANIFEST}.lux-sync.tmp")
if(EXISTS "${transaction}" OR IS_SYMLINK "${transaction}" OR
   EXISTS "${nextManifest}" OR IS_SYMLINK "${nextManifest}")
    message(FATAL_ERROR
        "Unfinished content synchronization. Check recovery plans and backups in ${transaction} and ${nextManifest}")
endif()
function(content_path relative output)
    if(relative STREQUAL "" OR relative STREQUAL "." OR IS_ABSOLUTE "${relative}" OR
       relative MATCHES "(^|/)[.][.](/|$)" OR relative MATCHES "[:\\\\]")
        message(FATAL_ERROR "Invalid generated content path: ${relative}")
    endif()
    set(candidate "${root}/${relative}")
    set(existing "${candidate}")
    while(NOT EXISTS "${existing}" AND NOT IS_SYMLINK "${existing}")
        cmake_path(GET existing PARENT_PATH existing)
    endwhile()
    set(ancestor "${existing}")
    while(NOT ancestor STREQUAL root)
        if(IS_SYMLINK "${ancestor}")
            message(FATAL_ERROR "Generated content path crosses a link: ${relative}")
        endif()
        cmake_path(GET ancestor PARENT_PATH ancestor)
    endwhile()
    file(REAL_PATH "${existing}" resolved)
    cmake_path(IS_PREFIX root "${resolved}" NORMALIZE inside)
    if(WIN32)
        string(TOLOWER "${resolved}" resolved)
        string(TOLOWER "${existing}" existing)
    endif()
    if(NOT inside OR NOT resolved STREQUAL existing)
        message(FATAL_ERROR "Generated content path crosses a link: ${relative}")
    endif()
    set(${output} "${candidate}" PARENT_SCOPE)
endfunction()

set(staleFiles)
set(staleDirectories)
set(previousPaths)
if(EXISTS "${MANIFEST}")
    file(STRINGS "${MANIFEST}" previousPaths)
    foreach(path IN LISTS previousPaths)
        if(path IN_LIST current OR IS_ABSOLUTE "${path}" OR
           path MATCHES "(^|/)[.][.](/|$)" OR path MATCHES "[:\\\\]")
            continue()
        endif()
        content_path("${path}" candidate)
        list(APPEND staleFiles "${path}")
        cmake_path(GET path PARENT_PATH directory)
        while(NOT directory STREQUAL "")
            list(APPEND staleDirectories "${directory}")
            cmake_path(GET directory PARENT_PATH directory)
        endwhile()
    endforeach()
    list(REMOVE_DUPLICATES staleDirectories)
    list(SORT staleDirectories ORDER DESCENDING)
endif()

function(validate_content source path)
    if(NOT EXISTS "${source}" OR IS_DIRECTORY "${source}")
        message(FATAL_ERROR "Missing generated content source: ${source}")
    endif()
    file(READ "${source}" firstByte LIMIT 1 HEX)
    content_path("${path}" destination)
    if(IS_DIRECTORY "${destination}")
        if(NOT path IN_LIST staleDirectories)
            message(FATAL_ERROR "Unmanaged directory blocks generated content: ${path}")
        endif()
        file(GLOB_RECURSE entries LIST_DIRECTORIES true RELATIVE "${root}" "${destination}/*")
        foreach(entry IN LISTS entries)
            content_path("${entry}" candidate)
            if(IS_DIRECTORY "${candidate}")
                set(owned "${staleDirectories}")
            else()
                set(owned "${staleFiles}")
            endif()
            if(NOT entry IN_LIST owned)
                message(FATAL_ERROR "Unmanaged content blocks directory replacement: ${entry}")
            endif()
        endforeach()
    endif()
    cmake_path(GET path PARENT_PATH directory)
    while(NOT directory STREQUAL "")
        content_path("${directory}" candidate)
        if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}" AND
           NOT directory IN_LIST staleFiles)
            message(FATAL_ERROR "File blocks generated content directory: ${directory}")
        endif()
        cmake_path(GET directory PARENT_PATH directory)
    endwhile()
endfunction()

validate_content("${PROJECT_MANIFEST}" project.luxproject)
foreach(path IN LISTS projectAssets)
    validate_content("${SOURCE_ROOT}/${path}" "${path}")
endforeach()
foreach(path IN LISTS scripts)
    validate_content("${MANAGED_ROOT}/${path}" "Scripts/${path}")
endforeach()

list(SORT current)
string(JOIN "\n" contents ${current})
string(APPEND contents "\n")

function(clean_transaction)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -r -- "${transaction}"
                    RESULT_VARIABLE cleaned)
    if(NOT cleaned STREQUAL "0")
        message(WARNING "Could not remove content synchronization data: ${transaction}")
    endif()
endfunction()

function(rollback_content reason)
    set(failures)
    list(REVERSE installedFiles)
    foreach(index IN LISTS installedFiles)
        list(GET changedFiles ${index} path)
        file(RENAME "${root}/${path}" "${transaction}/new/${index}" NO_REPLACE RESULT result)
        if(NOT result STREQUAL "0")
            list(APPEND failures "${path}: ${result}")
        endif()
    endforeach()
    list(REVERSE createdDirectories)
    foreach(path IN LISTS createdDirectories)
        file(GLOB entries "${root}/${path}/*")
        if(entries OR IS_SYMLINK "${root}/${path}")
            list(APPEND failures "${path}: directory changed during synchronization")
        else()
            list(FIND createdDirectories "${path}" index)
            file(RENAME "${root}/${path}" "${transaction}/rollback-dirs/${index}"
                 NO_REPLACE RESULT result)
            if(NOT result STREQUAL "0")
                list(APPEND failures "${path}: ${result}")
            endif()
        endif()
    endforeach()
    list(REVERSE movedDirectories)
    foreach(index IN LISTS movedDirectories)
        list(GET existingDirectories ${index} path)
        file(RENAME "${transaction}/old-dirs/${index}" "${path}" NO_REPLACE RESULT result)
        if(NOT result STREQUAL "0")
            list(APPEND failures "${path}: ${result}")
        endif()
    endforeach()
    foreach(index IN LISTS movedFiles)
        list(GET existingFiles ${index} path)
        file(RENAME "${transaction}/old-files/${index}" "${path}" NO_REPLACE RESULT result)
        if(NOT result STREQUAL "0")
            list(APPEND failures "${path}: ${result}")
        endif()
    endforeach()
    if(failures)
        string(JOIN "\n" details ${failures})
        message(FATAL_ERROR "${reason}\nRollback incomplete. Recovery files: ${transaction}\n${details}")
    endif()
    file(REMOVE "${nextManifest}")
    clean_transaction()
    message(FATAL_ERROR "${reason}\nPrevious generated content restored.")
endfunction()

set(existingFiles)
foreach(path IN LISTS staleFiles)
    content_path("${path}" candidate)
    if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}")
        file(REAL_PATH "${candidate}" actual)
        list(APPEND existingFiles "${actual}")
    endif()
endforeach()

set(existingDirectories)
foreach(path IN LISTS staleDirectories)
    content_path("${path}" candidate)
    if(IS_DIRECTORY "${candidate}")
        file(REAL_PATH "${candidate}" actual)
        list(APPEND existingDirectories "${actual}")
    endif()
endforeach()
list(REMOVE_DUPLICATES existingDirectories)
list(SORT existingDirectories ORDER DESCENDING)

set(changedFiles)
set(changedSources)
function(plan_content source path)
    content_path("${path}" destination)
    if(EXISTS "${destination}" AND NOT IS_DIRECTORY "${destination}")
        file(REAL_PATH "${destination}" actual)
        file(RELATIVE_PATH actualRelative "${root}" "${actual}")
        set(owned FALSE)
        if(path IN_LIST previousPaths OR actualRelative IN_LIST previousPaths)
            set(owned TRUE)
        endif()
        file(SIZE "${source}" sourceSize)
        file(SIZE "${destination}" destinationSize)
        if(sourceSize EQUAL destinationSize)
            file(SHA256 "${source}" sourceHash)
            file(SHA256 "${destination}" destinationHash)
            if(sourceHash STREQUAL destinationHash AND (actual STREQUAL destination OR NOT owned))
                return()
            endif()
        endif()
        if(NOT owned)
            message(FATAL_ERROR "Unmanaged file blocks generated content: ${path}")
        endif()
        set(existingFiles ${existingFiles} "${actual}" PARENT_SCOPE)
    endif()
    set(changedFiles ${changedFiles} "${path}" PARENT_SCOPE)
    set(changedSources ${changedSources} "${source}" PARENT_SCOPE)
endfunction()

plan_content("${PROJECT_MANIFEST}" project.luxproject)
foreach(path IN LISTS projectAssets)
    plan_content("${SOURCE_ROOT}/${path}" "${path}")
endforeach()
foreach(path IN LISTS scripts)
    plan_content("${MANAGED_ROOT}/${path}" "Scripts/${path}")
endforeach()
list(REMOVE_DUPLICATES existingFiles)

if(NOT changedFiles AND NOT existingFiles AND NOT existingDirectories AND EXISTS "${MANIFEST}")
    file(READ "${MANIFEST}" oldContents)
    if(oldContents STREQUAL contents)
        return()
    endif()
endif()

file(MAKE_DIRECTORY "${transaction}/new" "${transaction}/old-files"
     "${transaction}/old-dirs" "${transaction}/rollback-dirs")
string(JOIN "\n" filePlan ${existingFiles})
string(JOIN "\n" directoryPlan ${existingDirectories})
string(JOIN "\n" newFilePlan ${changedFiles})
file(WRITE "${transaction}/old-files.txt" "${filePlan}\n")
file(WRITE "${transaction}/old-dirs.txt" "${directoryPlan}\n")
file(WRITE "${transaction}/new-files.txt" "${newFilePlan}\n")
file(WRITE "${nextManifest}" "${contents}")

set(index 0)
foreach(source IN LISTS changedSources)
    file(COPY_FILE "${source}" "${transaction}/new/${index}" RESULT result)
    if(NOT result STREQUAL "0")
        rollback_content("Could not stage generated content ${source}: ${result}")
    endif()
    math(EXPR index "${index} + 1")
endforeach()

file(WRITE "${transaction}/prepared" "")
set(index 0)
foreach(path IN LISTS existingFiles)
    file(RENAME "${path}" "${transaction}/old-files/${index}" NO_REPLACE RESULT result)
    if(NOT result STREQUAL "0")
        rollback_content("Could not back up generated content ${path}: ${result}")
    endif()
    list(APPEND movedFiles "${index}")
    math(EXPR index "${index} + 1")
endforeach()

set(index 0)
foreach(path IN LISTS existingDirectories)
    file(GLOB entries "${path}/*")
    if(NOT entries)
        file(RENAME "${path}" "${transaction}/old-dirs/${index}" NO_REPLACE RESULT result)
        if(NOT result STREQUAL "0")
            rollback_content("Could not back up generated directory ${path}: ${result}")
        endif()
        list(APPEND movedDirectories "${index}")
        file(GLOB movedEntries "${transaction}/old-dirs/${index}/*")
        if(movedEntries)
            rollback_content("Generated directory changed during synchronization: ${path}")
        endif()
    endif()
    math(EXPR index "${index} + 1")
endforeach()

set(index 0)
foreach(path IN LISTS changedFiles)
    set(parents)
    cmake_path(GET path PARENT_PATH directory)
    while(NOT directory STREQUAL "" AND NOT IS_DIRECTORY "${root}/${directory}")
        list(PREPEND parents "${directory}")
        cmake_path(GET directory PARENT_PATH directory)
    endwhile()
    foreach(directory IN LISTS parents)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E make_directory "${root}/${directory}"
                        RESULT_VARIABLE result)
        if(NOT result STREQUAL "0")
            rollback_content("Could not create generated directory ${directory}: ${result}")
        endif()
        list(APPEND createdDirectories "${directory}")
    endforeach()
    file(RENAME "${transaction}/new/${index}" "${root}/${path}" NO_REPLACE RESULT result)
    if(NOT result STREQUAL "0")
        rollback_content("Could not install generated content ${path}: ${result}")
    endif()
    list(APPEND installedFiles "${index}")
    math(EXPR index "${index} + 1")
endforeach()

file(RENAME "${nextManifest}" "${MANIFEST}" RESULT result)
if(NOT result STREQUAL "0")
    rollback_content("Could not replace generated content manifest: ${result}")
endif()
clean_transaction()
)LUX";

constexpr const char* GameSource = R"LUX(#include <LuxEngine/LuxEngine.hpp>
#include <SDL3/SDL_filesystem.h>
#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Scene/SceneManager.hpp>
#include <LuxEngine/Runtime/ProjectManifest.hpp>
#include <LuxEngine/Runtime/ScenePlayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace le = LuxEngine;

std::filesystem::path FindStartScene(const std::filesystem::path& root) {
    std::filesystem::path target;
    std::string error;
    if (!le::ReadProjectStartScene(root, target, error))
        throw std::runtime_error(error);
    return target;
}

class Game final : public le::Application {
public:
    explicit Game(bool smokeTest)
        : le::Application({.name = "LuxGame", .width = 1280, .height = 720}),
          smokeTest_(smokeTest) {}

protected:
    void OnInit() override {
        le::Renderer2D::Init(&GetGpuDevice());
        le::Renderer3D::Init(&GetGpuDevice());
        le::PhysicsEngine::Init();
        const char* base = SDL_GetBasePath();
        if (!base)
            throw std::runtime_error("게임 실행 경로를 찾을 수 없습니다.");
        const auto basePath = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(base), std::strlen(base)));
        const auto sceneFile = FindStartScene(basePath / "Content");
        std::string error;
        scene_ = manager_.Load(sceneFile, error);
        if (!scene_)
            throw std::runtime_error(error.empty() ? "시작 씬을 열 수 없습니다." : error);
        scriptAssembly_ = basePath / "Content/Scripts/GameScripts.dll";
        EnsureScripts();
        le::Input::BindAction("Left", le::LuxKey::A);
        le::Input::BindAction("Left", le::LuxKey::Left);
        le::Input::BindAction("Right", le::LuxKey::D);
        le::Input::BindAction("Right", le::LuxKey::Right);
        le::Input::BindAction("Up", le::LuxKey::W);
        le::Input::BindAction("Up", le::LuxKey::Up);
        le::Input::BindAction("Down", le::LuxKey::S);
        le::Input::BindAction("Down", le::LuxKey::Down);
        le::Input::BindAction("Action", le::LuxKey::Space);
        le::Input::BindAction("Quit", le::LuxKey::Escape);
        FindActors();
    }

    void EnsureScripts() {
        if (scripts_.IsRunning())
            return;
        for (const auto id : scene_->GetComponentEntityIds<le::ScriptComponent>()) {
            if (!scene_->GetComponent<le::ScriptComponent>(id).enabled)
                continue;
            le::ScriptRuntimeConfig config;
            config.assemblyPath = scriptAssembly_;
            if (!scripts_.Initialize(*scene_, config))
                throw std::runtime_error(scripts_.GetLastError());
            return;
        }
    }

    void FindActors() {
        player_ = {};
        npc_ = {};
        goal_ = {};
        for (auto entity : scene_->GetEntities()) {
            if (!entity.HasComponent<le::TransformComponent>())
                continue;
            const auto* tagComponent = entity.TryGetComponent<le::TagComponent>();
            if (!tagComponent)
                continue;
            const auto& tag = tagComponent->tag;
            if (tag == "플레이어" || tag == "주인공" || tag == "블록")
                player_ = entity;
            if (tag == "NPC")
                npc_ = entity;
            if (tag == "목표")
                goal_ = entity;
        }
    }

    void TickStreaming(double dt) {
        streamingElapsed_ += dt;
        if (streamingElapsed_ < .1)
            return;
        streamingElapsed_ = 0;
        const auto observer = le::ResolveSceneStreamingObserver(*scene_, {}, player_.GetId());
        std::string error;
        bool changed = false;
        if (!manager_.UpdateStreaming(*scene_, {observer.x, observer.y}, error, &changed)) {
            if (error != streamingError_)
                le::Log::Error(error);
            streamingError_ = std::move(error);
        } else if (changed) {
            streamingError_.clear();
        }
        if (changed) {
            FindActors();
            EnsureScripts();
        }
    }

    void OnUpdate(double dt) override {
        if (le::Input::WasActionPressed("Quit"))
            Close();
        if (player_ && !player_.HasComponent<le::ScriptComponent>()) {
            const float horizontal = (le::Input::IsActionDown("Right") ? 1.0f : 0.0f) -
                                     (le::Input::IsActionDown("Left") ? 1.0f : 0.0f);
            const float vertical = (le::Input::IsActionDown("Up") ? 1.0f : 0.0f) -
                                   (le::Input::IsActionDown("Down") ? 1.0f : 0.0f);
            auto& transform = player_.GetComponent<le::TransformComponent>();
            if constexpr (LUX_GAME_TEMPLATE_KIND == 1) {
                if (auto* body = player_.TryGetComponent<le::Rigidbody2DComponent>()) {
                    body->velocity.x = horizontal * 4.0f;
                    if (body->isGrounded && le::Input::WasActionPressed("Action"))
                        body->velocity.y = 7.0f;
                }
            } else if constexpr (LUX_GAME_TEMPLATE_KIND == 4) {
                if (le::Input::WasActionPressed("Left"))
                    transform.position.x -= 1.0f;
                if (le::Input::WasActionPressed("Right"))
                    transform.position.x += 1.0f;
                if (le::Input::WasActionPressed("Up"))
                    transform.position.y += 1.0f;
                if (le::Input::WasActionPressed("Down"))
                    transform.position.y -= 1.0f;
                transform.position.x = std::clamp(transform.position.x, -3.0, 3.0);
                transform.position.y = std::clamp(transform.position.y, -3.0, 3.0);
                if (goal_) {
                    const auto& target = goal_.GetComponent<le::TransformComponent>().position;
                    if (transform.position.x == target.x && transform.position.y == target.y) {
                        if (auto* sprite = player_.TryGetComponent<le::SpriteRendererComponent>())
                            sprite->color = {0.42f, 0.95f, 0.58f, 1.0f};
                    }
                }
            } else {
                const float length = std::sqrt(horizontal * horizontal + vertical * vertical);
                if (length > 0.0f) {
                    transform.position.x += horizontal / length * 4.0f * static_cast<float>(dt);
                    transform.position.y += vertical / length * 4.0f * static_cast<float>(dt);
                }
            }
            if constexpr (LUX_GAME_TEMPLATE_KIND == 3) {
                if (npc_ && le::Input::WasActionPressed("Action")) {
                    const auto& target = npc_.GetComponent<le::TransformComponent>().position;
                    if (std::abs(target.x - transform.position.x) < 1.5f &&
                        std::abs(target.y - transform.position.y) < 1.5f) {
                        if (auto* sprite = npc_.TryGetComponent<le::SpriteRendererComponent>())
                            sprite->color = sprite->color.x > 0.5f
                                                ? le::Vec4{0.34f, 0.78f, 0.54f, 1.0f}
                                                : le::Vec4{0.82f, 0.52f, 0.38f, 1.0f};
                    }
                }
            }
        }
        TickStreaming(dt);
        if (scripts_.IsRunning() && !scripts_.Update(dt))
            throw std::runtime_error(scripts_.GetLastError());
        scene_->OnUpdate(dt);
        if (smokeTest_ && ++frames_ >= 10) {
            if (GetGpuDevice().GetCounters().validationErrors != 0)
                throw std::runtime_error("Vulkan 검증에서 렌더링 오류가 발견되었습니다.");
            Close();
        }
    }

    void OnRender(float) override {
        const auto& settings = scene_->GetSettings();
        GetGpuDevice().Clear(settings.clearColor.x, settings.clearColor.y, settings.clearColor.z,
                             settings.clearColor.w);
        le::uint32 width = 0, height = 0;
        const auto aspect = GetWindow().GetFramebufferSize(width, height) && width && height
                                ? static_cast<double>(width) / height
                                : 16.0 / 9;
        auto cameras = le::ResolveScenePlayerCameras(*scene_, aspect, {});
        if (!cameras.cameraEntity) {
            cameras.camera3D.SetPosition({0, 6, 12});
            cameras.camera3D.SetTarget({0, 0, 0});
        }
        if (cameras.perspective || !cameras.cameraEntity) {
            auto renderSettings = settings.render3D;
            if (!renderSettings.environmentPath.empty())
                renderSettings.environmentPath =
                    scene_->ResolveAssetPath(renderSettings.environmentPath);
            if (renderSettings.mode == le::Render3DMode::Pbr)
                le::Renderer3D::BeginScene(cameras.camera3D, renderSettings, settings.exposure);
            else
                le::Renderer3D::BeginScene(cameras.camera3D, le::DirectionalLight{});
            scene_->OnRender3D();
            le::Renderer3D::EndScene();
        }
        le::Renderer2D::BeginScene(cameras.camera2D);
        scene_->OnRender();
        le::Renderer2D::EndScene();
    }

    void OnShutdown() override {
        scripts_.Shutdown();
        le::Input::ClearBindings();
        manager_.ClearCache();
        le::PhysicsEngine::Shutdown();
        le::Renderer3D::Shutdown();
        le::Renderer2D::Shutdown();
    }

private:
    le::SceneManager manager_;
    le::ScriptRuntime scripts_;
    std::filesystem::path scriptAssembly_;
    std::shared_ptr<le::Scene> scene_;
    le::Entity player_;
    le::Entity npc_;
    le::Entity goal_;
    double streamingElapsed_ = 0;
    std::string streamingError_;
    bool smokeTest_ = false;
    int frames_ = 0;
};

int RunGame(int argc, char* argv[]) {
    Game game(argc > 1 && std::string_view(argv[1]) == "--smoke-test");
    if (!game.Initialize())
        return 1;
    const int result = game.Run();
    game.Shutdown();
    return result;
}
LUX_ENGINE_MAIN(RunGame)
)LUX";
constexpr const char* ScriptProject = R"LUX(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="LuxEngine.ScriptCore">
      <HintPath>$(LuxEngineScriptCore)</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
</Project>
)LUX";

constexpr const char* ScriptSource = R"LUX(using LuxEngine;

namespace Game {
    public sealed class Mover : EntityScript {
        public float Speed = 4.0f;

        protected override void OnUpdate(double deltaTime) {
            Transform transform = Entity.Transform;
            float horizontal = (Input.IsKeyPressed(LuxKey.D) ? 1.0f : 0.0f)
                - (Input.IsKeyPressed(LuxKey.A) ? 1.0f : 0.0f);
            transform.Position.X += horizontal * Speed * (float)deltaTime;
            Entity.Transform = transform;
        }
    }
}
)LUX";

constexpr const char* ScriptFormatting = R"LUX(root = true

[*.cs]
indent_style = space
indent_size = 4
end_of_line = lf
charset = utf-8
insert_final_newline = true
csharp_new_line_before_open_brace = none
csharp_new_line_before_else = false
csharp_new_line_before_catch = false
csharp_new_line_before_finally = false
csharp_style_namespace_declarations = block_scoped:warning
csharp_style_expression_bodied_methods = false:warning
csharp_preserve_single_line_blocks = false
csharp_preserve_single_line_statements = false
)LUX";
} // namespace

bool CreateTemplateFiles(const std::filesystem::path& directory, ProjectTemplate projectTemplate,
                         std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory / "Scripts", ec);
    if (ec) {
        error = "프로젝트 소스 폴더를 만들 수 없습니다.";
        return false;
    }
    std::filesystem::create_directories(directory / "Source", ec);
    if (ec) {
        error = "프로젝트 소스 폴더를 만들 수 없습니다.";
        return false;
    }
    const std::pair<const char*, const char*> scriptFiles[] = {
        {"Scripts/GameScripts.csproj", ScriptProject},
        {"Scripts/Mover.cs", ScriptSource},
        {".editorconfig", ScriptFormatting},
        {"SyncContent.cmake", SyncContent},
        {"Scripts/NuGet.Config",
         "<configuration><packageSources><clear /></packageSources></configuration>\n"}};
    for (const auto& [path, content] : scriptFiles) {
        std::ofstream file(directory / path, std::ios::binary);
        file << content;
        file.close();
        if (!file) {
            error = "C# 프로젝트 파일을 저장할 수 없습니다.";
            return false;
        }
    }
    std::string cmake = GameCMake;
    const auto marker = cmake.find("@KIND@");
    cmake.replace(marker, 6, std::to_string(static_cast<int>(projectTemplate)));
    std::ofstream buildFile(directory / "CMakeLists.txt", std::ios::binary);
    std::ofstream sourceFile(directory / "Source" / "Main.cpp", std::ios::binary);
    buildFile << cmake;
    sourceFile << GameSource;
    buildFile.close();
    sourceFile.close();
    if (!buildFile || !sourceFile) {
        error = "프로젝트 시작 코드를 저장할 수 없습니다.";
        return false;
    }
    return true;
}

} // namespace LuxEditor
