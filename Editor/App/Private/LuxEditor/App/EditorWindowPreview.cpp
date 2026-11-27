#include <LuxEditor/App/EditorWindow.hpp>
#include <LuxEditor/Debug/DebugPanel.hpp>
#include <LuxEditor/Scripting/ScriptWorkspace.hpp>
#include <LuxEditor/Viewport/GameViewport.hpp>
#include <LuxEditor/Viewport/SceneViewport2D.hpp>
#include <algorithm>
#include <cmath>

namespace LuxEditor {
void EditorWindow::StartPreview() {
    if (scriptWorkspace_->IsBuilding() || scriptWorkspace_->IsDebugging() || buildProcess_) {
        Log(LUX_UI_TEXT("빌드가 끝난 뒤 실행해 주세요."), true);
        return;
    }
    CancelViewportEdit();
    if (preview_.IsActive())
        return;
    StopAnimationPlayback();
    if (const auto failure = preview_.Start(document_.GetScene(), ResolvePlayAssembly())) {
        Log(LuxUI::String::fromUtf8(*failure), true);
        return;
    }
    previewTimer_.setInterval(std::max(
        1, static_cast<int>(std::lround(preview_.GetScene()->GetSettings().fixedStep * 1000))));
    gameCanvas_->SetPaused(false);
    gameCanvas_->SetInputEnabled(true);
    gameCanvas_->RefreshScene();
    RefreshModelingWorkspace();
    viewportTabs_->setCurrentWidget(gameCanvas_);
    gameCanvas_->setFocus(LuxUI::Flags::OtherFocusReason);
    previewStartAction_->setEnabled(false);
    previewPauseAction_->setEnabled(true);
    previewStepAction_->setEnabled(true);
    previewStopAction_->setEnabled(true);
    previewClock_.Reset();
    previewTimer_.start();
    Log(LUX_UI_TEXT("게임 실행을 시작했습니다."));
}

void EditorWindow::StopPreview() {
    if (!preview_.IsActive())
        return;
    previewTimer_.stop();
    preview_.Stop();
    gameCanvas_->SetInputEnabled(false);
    gameCanvas_->SetPaused(false);
    previewStartAction_->setEnabled(true);
    previewPauseAction_->setEnabled(false);
    previewStepAction_->setEnabled(false);
    previewStopAction_->setEnabled(false);
    RefreshAll();
    Log(LUX_UI_TEXT("게임 실행을 중지했습니다."));
}

void EditorWindow::PausePreview() {
    preview_.Pause();
    previewTimer_.stop();
    gameCanvas_->SetPaused(true);
}

void EditorWindow::TogglePreviewPause() {
    if (!preview_.IsActive())
        return;
    if (preview_.IsRunning()) {
        PausePreview();
        return;
    }
    preview_.Resume();
    gameCanvas_->SetPaused(false);
    previewClock_.Reset();
    previewTimer_.start();
}

void EditorWindow::StepPreview() {
    if (!preview_.IsActive())
        StartPreview();
    if (!preview_.IsActive())
        return;
    PausePreview();
    preview_.Step(GetPreviewView(), [this](const auto& update) { PresentPreviewUpdate(update); });
}

void EditorWindow::AdvancePreview() {
    if (!preview_.IsRunning())
        return;
    const auto elapsed = previewClock_.ElapsedSeconds();
    previewClock_.Reset();
    preview_.Advance(elapsed, GetPreviewView(),
                     [this](const auto& update) { PresentPreviewUpdate(update); });
}

LuxEngine::ScenePlayerView EditorWindow::GetPreviewView() const {
    LuxEngine::ScenePlayerView view;
    canvas_->GetView(view.centerX, view.centerY, view.halfHeight);
    return view;
}

void EditorWindow::PresentPreviewUpdate(const PreviewSession::Update& update) {
    if (!update.streamingError.empty())
        Log(LuxUI::String::fromUtf8(update.streamingError), true);
    if (update.failure) {
        PausePreview();
        Log(LUX_UI_TEXT("게임 실행 오류: %1 — 실행을 일시정지했습니다.")
                .arg(LuxUI::String::fromUtf8(*update.failure)),
            true);
        gameCanvas_->RefreshScene();
        return;
    }
    auto& scene = *preview_.GetScene();
    const auto selected = document_.GetSelectedId(document_.GetActiveTabId());
    const auto frame = preview_.Frame();
    debugPanel_->UpdateScene(scene, selected.value_or(0), frame, preview_.LastUpdateMs());
    const auto& collisions2D = scene.GetCollisionEvents2D();
    const auto& collisions3D = scene.GetCollisionEvents3D();
    for (const auto& event : collisions2D)
        debugPanel_->AddLog(LUX_UI_TEXT("프레임 %1: #%2 ↔ #%3 %4")
                                .arg(frame)
                                .arg(event.entityA)
                                .arg(event.entityB)
                                .arg(event.isTrigger ? LUX_UI_TEXT("트리거") : LUX_UI_TEXT("충돌")),
                            false);
    for (const auto& event : collisions3D)
        debugPanel_->AddLog(LUX_UI_TEXT("3D 프레임 %1: #%2 ↔ #%3 %4")
                                .arg(frame)
                                .arg(event.entityA)
                                .arg(event.entityB)
                                .arg(event.isTrigger ? LUX_UI_TEXT("트리거") : LUX_UI_TEXT("충돌")),
                            false);
    if (breakOnCollisionAction_->isChecked() && (!collisions2D.empty() || !collisions3D.empty())) {
        PausePreview();
        Log(LUX_UI_TEXT("충돌 이벤트에서 게임 실행을 일시정지했습니다."));
    }
    gameCanvas_->RefreshScene();
}

LuxEngine::Scene* EditorWindow::GetRuntimeScene() noexcept {
    return preview_.GetScene();
}

const LuxEngine::Scene* EditorWindow::GetRuntimeScene() const noexcept {
    return preview_.GetScene();
}
} // namespace LuxEditor
