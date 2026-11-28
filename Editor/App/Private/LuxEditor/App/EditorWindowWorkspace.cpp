#include <LuxEditor/Animation/AnimationAssetEditor.hpp>
#include <LuxEditor/App/EditorWindow.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scripting/ScriptWorkspace.hpp>
#include <LuxEditor/Ui/IconFactory.hpp>
#include <LuxEditor/Viewport/SceneViewport2D.hpp>
#include <LuxEditor/Viewport/SceneViewport3D.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxUI/Widgets.hpp>
#include <algorithm>
#include <map>

namespace LuxEditor {
void EditorWindow::BuildWorkspaceChrome() {
    auto* workspaceBar = new LuxUI::ToolBar(LUX_UI_TEXT("작업 공간"), this);
    workspaceBar->setObjectName(LUX_UI_TEXT("workspaceToolbar"));
    workspaceBar->setMovable(false);
    auto* brand = new LuxUI::Label(LUX_UI_TEXT("  LUX  "), workspaceBar);
    brand->setObjectName(LUX_UI_TEXT("editorBrand"));
    brand->setPixmap(MakeBrandIcon(true).pixmap(30, 30));
    brand->setFixedSize(42, 38);
    brand->setAlignment(LuxUI::Flags::AlignCenter);
    brand->setToolTip(LUX_UI_TEXT("LuxEngine"));
    workspaceBar->addWidget(brand);
    workspaceTabs_ = new LuxUI::TabBar(workspaceBar);
    workspaceTabs_->setObjectName(LUX_UI_TEXT("workspaceTabs"));
    workspaceTabs_->addTab(MakeEditorIcon(EditorIcon::Mesh), LUX_UI_TEXT("레벨 에디터"));
    workspaceTabs_->addTab(MakeEditorIcon(EditorIcon::Play), LUX_UI_TEXT("애니메이션"));
    workspaceTabs_->addTab(MakeEditorIcon(EditorIcon::Console), LUX_UI_TEXT("스크립트"));
    workspaceTabs_->setExpanding(false);
    workspaceBar->addWidget(workspaceTabs_);
    auto* mainToolbar = findChild<LuxUI::ToolBar*>(LUX_UI_TEXT("lux-main-toolbar"));
    insertToolBar(mainToolbar, workspaceBar);
    insertToolBarBreak(mainToolbar);
    connect(workspaceTabs_, &LuxUI::TabBar::currentChanged, this,
            [this](int index) { SetWorkspace(static_cast<Workspace>(index)); });
    auto* stretch = new LuxUI::Widget(workspaceBar);
    stretch->setSizePolicy(LuxUI::SizePolicy::Expanding, LuxUI::SizePolicy::Preferred);
    workspaceBar->addWidget(stretch);
    // One persistent play strip is shared by level, animation and script documents.
    for (auto* action :
         {previewStartAction_, previewPauseAction_, previewStepAction_, previewStopAction_}) {
        mainToolbar->removeAction(action);
        workspaceBar->addAction(action);
    }
    if (auto* play = workspaceBar->widgetForAction(previewStartAction_))
        play->setObjectName(LUX_UI_TEXT("playButton"));
    workspaceBar->addSeparator();
    projectLabel_ = new LuxUI::Label(LUX_UI_TEXT("프로젝트 없음"), workspaceBar);
    projectLabel_->setMinimumWidth(0);
    projectLabel_->setMaximumWidth(220);
    workspaceBar->addWidget(projectLabel_);
    auto* commands = new LuxUI::ToolButton(workspaceBar);
    commands->setIcon(MakeEditorIcon(EditorIcon::Search));
    commands->setToolTip(LUX_UI_TEXT("명령 검색 (Ctrl+P)"));
    connect(commands, &LuxUI::ToolButton::clicked, this, [this] { OpenCommandPalette(); });
    workspaceBar->addWidget(commands);

    // Animation is a document workspace, not a small console tab competing with the viewport.
    if (auto* tabs = findChild<LuxUI::TabWidget*>(LUX_UI_TEXT("animationDocuments"))) {
        connect(tabs, &LuxUI::TabWidget::currentChanged, this, [this] { UpdateChrome(); });
        if (auto* asset = dynamic_cast<AnimationAssetEditor*>(tabs->widget(0)))
            asset->StateChanged = [this] { UpdateChrome(); };
    }
    removeDockWidget(animationDock_);
    animationDock_->setFeatures(LuxUI::DockWidget::NoDockWidgetFeatures);
    animationDock_->setTitleBarWidget(new LuxUI::Widget(animationDock_));
    workspaceStack_->addWidget(animationDock_);
    animationDock_->hide();
    scriptWorkspace_ = new ScriptWorkspace(workspaceStack_);
    scriptWorkspace_->debugRequested = [this] { StartScriptDebug(); };
    scriptWorkspace_->debugStopped = [this] {
        if (playerProcess_ && playerProcess_->property("scriptDebugger").toBool())
            StopSceneWindow();
    };
    workspaceStack_->addWidget(scriptWorkspace_);

    auto* detailsLayout = qobject_cast<LuxUI::VBoxLayout*>(inspectorBody_->layout());
    const auto groups = inspectorBody_->findChildren<LuxUI::GroupBox*>(
        LuxUI::String(), LuxUI::Flags::FindDirectChildrenOnly);
    for (auto* group : groups) {
        const int index = detailsLayout->indexOf(group);
        auto* section = new LuxUI::Widget(inspectorBody_);
        section->setObjectName(LUX_UI_TEXT("componentSection"));
        section->setProperty("sectionName", group->title());
        auto* sectionLayout = new LuxUI::VBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(0);
        auto* header = new LuxUI::ToolButton(section);
        header->setObjectName(LUX_UI_TEXT("componentHeader"));
        header->setText(group->title());
        header->setToolButtonStyle(LuxUI::Flags::ToolButtonTextBesideIcon);
        header->setArrowType(LuxUI::Flags::DownArrow);
        header->setCheckable(true);
        header->setChecked(true);
        header->setSizePolicy(LuxUI::SizePolicy::Expanding, LuxUI::SizePolicy::Fixed);
        sectionLayout->addWidget(header);
        detailsLayout->removeWidget(group);
        group->setProperty("componentTitle", group->title());
        group->setTitle({});
        sectionLayout->addWidget(group);
        detailsLayout->insertWidget(index, section);
        connect(header, &LuxUI::ToolButton::toggled, section, [group, header](bool expanded) {
            group->setVisible(expanded);
            header->setArrowType(expanded ? LuxUI::Flags::DownArrow : LuxUI::Flags::RightArrow);
        });
    }

    auto* addComponent = new LuxUI::ToolButton(inspectorBody_);
    addComponent->setObjectName(LUX_UI_TEXT("addComponent"));
    addComponent->setText(LUX_UI_TEXT("+ 컴포넌트 추가"));
    addComponent->setPopupMode(LuxUI::ToolButton::InstantPopup);
    addComponent->setSizePolicy(LuxUI::SizePolicy::Expanding, LuxUI::SizePolicy::Fixed);
    auto* components = new LuxUI::Menu(addComponent);
    const std::pair<const char*, LuxUI::CheckBox*> choices[] = {
        {"Sprite Renderer", spriteEnabled_},
        {"Mesh Renderer", meshEnabled_},
        {"Tilemap", tilemapEnabled_},
        {"Rigidbody 2D", bodyEnabled_},
        {"Box Collider 2D", colliderEnabled_}};
    for (const auto& [name, field] : choices)
        components->addAction(LuxUI::String::fromLatin1(name), this,
                              [field] { field->setChecked(true); });
    components->addAction(LUX_UI_TEXT("Jolt Rigidbody 3D"), this,
                          [this] { body3DEnabled_->setChecked(true); });
    components->addAction(LUX_UI_TEXT("Jolt Box Collider 3D"), this,
                          [this] { collider3DEnabled_->setChecked(true); });
    components->addAction(LUX_UI_TEXT("Camera"), this,
                          [this] { cameraEnabled_->setChecked(true); });
    components->addAction(LUX_UI_TEXT("애니메이션 클립"), this, [this] {
        animationEnabled_->setChecked(true);
        SetWorkspace(true);
    });
    addComponent->setMenu(components);
    detailsLayout->insertWidget(1, addComponent);

    const auto drawer = [this](const LuxUI::String& text, EditorIcon icon, size_t index,
                               const char* name) {
        auto* button = new LuxUI::ToolButton(this);
        button->setObjectName(LuxUI::String::fromLatin1(name));
        button->setText(text);
        button->setIcon(MakeEditorIcon(icon));
        button->setToolButtonStyle(LuxUI::Flags::ToolButtonTextBesideIcon);
        button->setCheckable(true);
        statusBar()->addWidget(button);
        connect(button, &LuxUI::ToolButton::clicked, this, [this, index] { ToggleDrawer(index); });
        connect(docks_[index], &LuxUI::DockWidget::visibilityChanged, button,
                &LuxUI::ToolButton::setChecked);
        return button;
    };
    auto* content = drawer(LUX_UI_TEXT("콘텐츠"), EditorIcon::Folder, 1, "contentDrawerButton");
    content->setToolTip(LUX_UI_TEXT("콘텐츠 브라우저 (Ctrl+Space)"));
    auto* contentShortcut = new LuxUI::Action(this);
    contentShortcut->setObjectName(LUX_UI_TEXT("toggleContentBrowser"));
    contentShortcut->setShortcut(LuxUI::KeySequence(LuxUI::Flags::CTRL | LuxUI::Flags::Key_Space));
    addAction(contentShortcut);
    connect(contentShortcut, &LuxUI::Action::triggered, content, &LuxUI::ToolButton::click);
    drawer(LUX_UI_TEXT("출력 로그"), EditorIcon::Console, 3, "outputLogButton");
    drawer(LUX_UI_TEXT("문제"), EditorIcon::Bug, 4, "problemsDrawerButton");
    sceneStatus_ = new LuxUI::Label(this);
    sceneStatus_->setObjectName(LUX_UI_TEXT("sceneStatus"));
    sceneStatus_->setAlignment(LuxUI::Flags::AlignRight | LuxUI::Flags::AlignVCenter);
    statusBar()->addPermanentWidget(sceneStatus_, 1);
    statusBar()->setSizeGripEnabled(true);
}

void EditorWindow::SetWorkspace(bool animation) {
    SetWorkspace(animation ? Workspace::Animation : Workspace::Level);
}

void EditorWindow::SetWorkspace(Workspace workspace) {
    if (!workspaceStack_ || !animationDock_ || !scriptWorkspace_)
        return;
    const int target = static_cast<int>(workspace);
    const LuxUI::SignalBlocker blocker(workspaceTabs_);
    workspaceTabs_->setCurrentIndex(target);
    const int previous = workspaceStack_->currentIndex();
    if (previous == target)
        return;
    const bool level = workspace == Workspace::Level;
    for (auto* action : toolActions_)
        action->setEnabled(level);
    for (const auto* id :
         {"addEntity", "addMeshEntity", "addTilemap", "deleteEntity", "saveSceneAs"})
        if (auto* action = findChild<LuxUI::Action*>(LuxUI::String::fromLatin1(id)))
            action->setEnabled(level);
    if (undoAction_)
        undoAction_->setEnabled(level ? undo_->canUndo() : workspace == Workspace::Script);
    if (redoAction_)
        redoAction_->setEnabled(level ? undo_->canRedo() : workspace == Workspace::Script);
    CancelViewportEdit();
    canvas_->CancelInteraction();
    canvas3D_->CancelInteraction();
    StopAnimationPlayback();
    if (previous == 0)
        levelLayout_ = saveState(6);
    if (workspace != Workspace::Level) {
        if (modelingDock_)
            modelingDock_->hide();
        if (auto* toolbar = findChild<LuxUI::ToolBar*>(LUX_UI_TEXT("lux-main-toolbar")))
            toolbar->hide();
        for (auto* dock : docks_)
            dock->hide();
        if (profilerDock_)
            profilerDock_->hide();
        workspaceStack_->setCurrentIndex(target);
        if (workspace == Workspace::Animation)
            animationDock_->show();
        else {
            scriptWorkspace_->SetProjectRoot(project_ ? project_->GetFile().parent_path()
                                                      : std::filesystem::path{});
            scriptWorkspace_->SetExecutionActive(preview_.IsActive() || buildProcess_);
        }
    } else {
        workspaceStack_->setCurrentIndex(0);
        if (!levelLayout_.isEmpty())
            restoreState(levelLayout_, 6);
        levelLayout_.clear();
        if (auto* toolbar = findChild<LuxUI::ToolBar*>(LUX_UI_TEXT("lux-main-toolbar")))
            toolbar->show();
    }
    if (auto* saveAs = findChild<LuxUI::Action*>(LUX_UI_TEXT("saveSceneAs")))
        saveAs->setEnabled(workspace != Workspace::Script);
    UpdateChrome();
}

void EditorWindow::OpenScriptFile(const std::filesystem::path& path, int line, int column) {
    std::string error;
    if (!scriptWorkspace_->OpenFile(path, error, line, column)) {
        ShowError(LUX_UI_TEXT("스크립트 열기"), LuxUI::String::fromUtf8(error));
        return;
    }
    SetWorkspace(Workspace::Script);
}

void EditorWindow::ToggleDrawer(size_t index) {
    if (index >= docks_.size())
        return;
    const bool close = docks_[index]->isVisible();
    for (size_t drawer : {size_t{1}, size_t{3}, size_t{4}, size_t{7}})
        docks_[drawer]->hide();
    if (!close) {
        docks_[index]->show();
        docks_[index]->raise();
        resizeDocks({docks_[index]}, {270}, LuxUI::Flags::Vertical);
    }
}

void EditorWindow::FilterDetails(const LuxUI::String& text) {
    for (auto* section :
         inspectorBody_->findChildren<LuxUI::Widget*>(LUX_UI_TEXT("componentSection"))) {
        bool matches =
            text.trimmed().isEmpty() || section->property("sectionName")
                                            .toString()
                                            .contains(text, LuxUI::Flags::CaseInsensitive);
        for (auto* label : section->findChildren<LuxUI::Label*>())
            matches |= label->text().contains(text, LuxUI::Flags::CaseInsensitive);
        for (auto* field : section->findChildren<LuxUI::CheckBox*>())
            matches |= field->text().contains(text, LuxUI::Flags::CaseInsensitive);
        const auto entity = document_.GetSelected();
        const auto name = section->property("sectionName").toString();
        bool available = static_cast<bool>(entity);
        if (entity) {
            if (name == LuxUI::Latin1String("Sprite Renderer"))
                available = entity.HasComponent<LuxEngine::SpriteRendererComponent>();
            else if (name == LuxUI::Latin1String("Mesh Renderer"))
                available = entity.HasComponent<LuxEngine::MeshRendererComponent>();
            else if (name == LuxUI::Latin1String("Tilemap"))
                available = entity.HasComponent<LuxEngine::TilemapComponent>();
            else if (name == LuxUI::Latin1String("Physics 2D"))
                available = entity.HasComponent<LuxEngine::Rigidbody2DComponent>() ||
                            entity.HasComponent<LuxEngine::BoxCollider2DComponent>();
            else if (name == LUX_UI_TEXT("씬 참조"))
                available = entity.HasComponent<LuxEngine::SceneReferenceComponent>();
            else if (name == LuxUI::Latin1String("Camera"))
                available = entity.HasComponent<LuxEngine::CameraComponent>();
            else if (name == LuxUI::Latin1String("Transform"))
                available = entity.HasComponent<LuxEngine::TransformComponent>();
        }
        section->setVisible(matches && available);
    }
}

void EditorWindow::RefreshAssetFolders() {
    if (!assetFolders_)
        return;
    const LuxUI::SignalBlocker blocker(assetFolders_);
    assetFolders_->clear();
    if (!project_) {
        assetProjectRoot_.clear();
        assetFolder_.clear();
        return;
    }
    const auto root = project_->GetFile().parent_path();
    if (root != assetProjectRoot_) {
        assetProjectRoot_ = root;
        assetFolder_.clear();
    }
    std::map<std::filesystem::path, LuxUI::TreeWidgetItem*> folders;
    auto* all = new LuxUI::TreeWidgetItem(assetFolders_, {LUX_UI_TEXT("모든 콘텐츠")});
    all->setIcon(0, MakeEditorIcon(EditorIcon::Folder));
    folders.emplace(std::filesystem::path{}, all);
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error),
        end;
    for (; !error && iterator != end && folders.size() < 512; iterator.increment(error)) {
        if (!iterator->is_directory(error))
            continue;
        const auto name = PathUtf8(iterator->path().filename());
        if (name.starts_with('.') || name == "out" || name == "build" ||
            name.starts_with("cmake-build")) {
            iterator.disable_recursion_pending();
            continue;
        }
        const auto relative = iterator->path().lexically_relative(root);
        const auto parent = folders.find(relative.parent_path());
        auto* item = new LuxUI::TreeWidgetItem(parent == folders.end() ? all : parent->second,
                                               {LuxUI::String::fromUtf8(name)});
        item->setData(0, LuxUI::Flags::UserRole,
                      LuxUI::String::fromUtf8(GenericPathUtf8(relative)));
        item->setIcon(0, MakeEditorIcon(EditorIcon::Folder));
        folders.emplace(relative, item);
    }
    const auto selected = folders.find(assetFolder_);
    if (selected == folders.end())
        assetFolder_.clear();
    assetFolders_->setCurrentItem(selected == folders.end() ? all : selected->second);
    all->setExpanded(true);
    assetBreadcrumb_->setText(assetFolder_.empty()
                                  ? LuxUI::String::fromUtf8(project_->GetName())
                                  : LuxUI::String::fromUtf8(GenericPathUtf8(assetFolder_)));
}
} // namespace LuxEditor
