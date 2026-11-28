#include <LuxEditor/Scene/PostProcessControls.hpp>
#include <algorithm>

namespace LuxEditor {
namespace {
using namespace LuxUI;
using LuxEngine::PostProcessVolume;
using LuxEngine::Render3DSettings;

template <class T>
DoubleSpinBox* Number(FormLayout& form, const String& label, T& target, double lo, double hi,
                      int digits = 3) {
    auto* field = new DoubleSpinBox(form.parentWidget());
    field->setDecimals(digits);
    field->setRange(lo, hi);
    field->setValue(target);
    form.addRow(label, field);
    Object::connect(field, &DoubleSpinBox::valueChanged, field,
                    [&target](double edited) { target = static_cast<T>(edited); });
    return field;
}

void Toggle(FormLayout& form, const String& label, bool& target) {
    auto* field = new CheckBox(label, form.parentWidget());
    field->setChecked(target);
    form.addRow(field);
    Object::connect(field, &CheckBox::toggled, field, [&target](bool value) { target = value; });
}

void Triplet(FormLayout& form, const String& title, std::array<float, 3>& values, double lo,
             double hi) {
    auto* row = new HBoxLayout();
    for (size_t i = 0; i < 3; ++i) {
        auto* field = new DoubleSpinBox(form.parentWidget());
        field->setPrefix(String::fromUtf8(i == 0 ? "R " : i == 1 ? "G " : "B "));
        field->setDecimals(3);
        field->setRange(lo, hi);
        field->setValue(values[i]);
        Object::connect(field, &DoubleSpinBox::valueChanged, field,
                        [&values, i](double x) { values[i] = float(x); });
        row->addWidget(field);
    }
    form.addRow(title, row);
}

bool EditVolume(Widget& parent, PostProcessVolume& volume) {
    Dialog editor(&parent);
    editor.setWindowTitle(LUX_UI_TEXT("후처리 볼륨"));
    auto* form = new FormLayout(&editor);
    Toggle(*form, LUX_UI_TEXT("월드 전체에 적용"), volume.global);
    Number(*form, LUX_UI_TEXT("우선순위"), volume.priority, -2147483648., 2147483647., 0);
    for (size_t i = 0; i < 3; ++i) {
        const auto axis = LUX_UI_TEXT("XYZ").at(i);
        Number(*form, LUX_UI_TEXT("중심 %1 (m)").arg(axis), volume.center[i], -1e12, 1e12);
        Number(*form, LUX_UI_TEXT("반폭 %1 (m)").arg(axis), volume.halfExtent[i], 0, 1e12);
    }
    Number(*form, LUX_UI_TEXT("바깥 전환 거리 (m)"), volume.blendDistance, 0, 1e9);
    Number(*form, LUX_UI_TEXT("가중치"), volume.weight, 0, 1);
    const auto overrideField = [&](const String& label, std::optional<float>& value, float fallback,
                                   float lo, float hi) {
        auto* enabled = new CheckBox(label, &editor);
        auto* field = new DoubleSpinBox(&editor);
        field->setDecimals(3);
        field->setRange(lo, hi);
        field->setValue(value.value_or(fallback));
        enabled->setChecked(value.has_value());
        field->setEnabled(value.has_value());
        form->addRow(enabled, field);
        Object::connect(enabled, &CheckBox::toggled, field, [field, &value](bool on) {
            field->setEnabled(on);
            if (on)
                value = float(field->value());
            else
                value.reset();
        });
        Object::connect(field, &DoubleSpinBox::valueChanged, field,
                        [&value](double x) { value = float(x); });
    };
    overrideField(LUX_UI_TEXT("노출 보정 (EV)"), volume.compensationEV, 0, -16, 16);
    overrideField(LUX_UI_TEXT("블룸 강도"), volume.bloomIntensity, 0, 0, 2);
    overrideField(LUX_UI_TEXT("지역 노출"), volume.localExposure, 0, 0, 1);
    overrideField(LUX_UI_TEXT("채도"), volume.saturation, 1, 0, 2);
    overrideField(LUX_UI_TEXT("화이트 밸런스 (K)"), volume.temperatureK, 6500, 2500, 25000);
    overrideField(LUX_UI_TEXT("초점 거리 (m)"), volume.focusDistance, 10, .001f, 1e8f);
    auto* buttons = new DialogButtonBox(DialogButtonBox::Ok | DialogButtonBox::Cancel, &editor);
    form->addRow(buttons);
    Object::connect(buttons, &DialogButtonBox::accepted, &editor, &Dialog::accept);
    Object::connect(buttons, &DialogButtonBox::rejected, &editor, &Dialog::reject);
    return editor.exec() == Dialog::Accepted;
}

void Volumes(FormLayout& form, Widget& host, std::vector<PostProcessVolume>& boxes) {
    auto* selected = new ComboBox(&host);
    form.addRow(selected);
    auto* actions = new HBoxLayout();
    const auto action = [&](const String& text) {
        auto* button = new PushButton(text, &host);
        actions->addWidget(button);
        return button;
    };
    auto* add = action(LUX_UI_TEXT("추가"));
    auto* edit = action(LUX_UI_TEXT("편집"));
    auto* remove = action(LUX_UI_TEXT("삭제"));
    form.addRow(actions);
    const auto refresh = [selected, edit, remove, add, &boxes](int index) {
        selected->clear();
        for (size_t i = 0; i < boxes.size(); ++i)
            selected->addItem(LUX_UI_TEXT("%1 · %2 · %3")
                                  .arg(i + 1)
                                  .arg(boxes[i].global ? LUX_UI_TEXT("전역") : LUX_UI_TEXT("지역"))
                                  .arg(boxes[i].priority));
        selected->setCurrentIndex(std::clamp(index, 0, std::max(0, selected->count() - 1)));
        edit->setEnabled(!boxes.empty());
        remove->setEnabled(!boxes.empty());
        add->setEnabled(boxes.size() < 64);
    };
    refresh(0);
    Object::connect(add, &PushButton::clicked, add, [refresh, &host, &boxes] {
        PostProcessVolume box;
        if (EditVolume(host, box)) {
            boxes.push_back(std::move(box));
            refresh(int(boxes.size()) - 1);
        }
    });
    Object::connect(edit, &PushButton::clicked, edit, [selected, refresh, &host, &boxes] {
        const int i = selected->currentIndex();
        if (i < 0)
            return;
        auto edited = boxes[i];
        if (EditVolume(host, edited)) {
            boxes[i] = std::move(edited);
            refresh(i);
        }
    });
    Object::connect(remove, &PushButton::clicked, remove, [selected, refresh, &boxes] {
        const int i = selected->currentIndex();
        if (i >= 0) {
            boxes.erase(boxes.begin() + i);
            refresh(i);
        }
    });
}
} // namespace

void AddPostProcessControls(VBoxLayout& layout, Widget& host, Render3DSettings& s) {
    const auto group = [&](const String& title) {
        auto* box = new GroupBox(title, &host);
        layout.addWidget(box);
        return new FormLayout(box);
    };
    auto* camera = group(LUX_UI_TEXT("카메라와 노출"));
    auto* debug = new ComboBox(&host);
    debug->addItems({LUX_UI_TEXT("일반"),
                     LUX_UI_TEXT("노출 히스토그램"),
                     LUX_UI_TEXT("HDR 휘도"),
                     LUX_UI_TEXT("텍스처 밉"),
                     LUX_UI_TEXT("기본 색"),
                     LUX_UI_TEXT("월드 노멀"),
                     LUX_UI_TEXT("기하 노멀"),
                     LUX_UI_TEXT("거칠기"),
                     LUX_UI_TEXT("금속성"),
                     LUX_UI_TEXT("재질 AO"),
                     LUX_UI_TEXT("GTAO"),
                     LUX_UI_TEXT("벤트 노멀"),
                     LUX_UI_TEXT("직접광"),
                     LUX_UI_TEXT("간접광"),
                     LUX_UI_TEXT("환경 반사광"),
                     LUX_UI_TEXT("발광"),
                     LUX_UI_TEXT("씬 깊이"),
                     LUX_UI_TEXT("선형 깊이"),
                     LUX_UI_TEXT("깊이 피라미드"),
                     LUX_UI_TEXT("그림자 맵"),
                     LUX_UI_TEXT("그림자 캐스케이드"),
                     LUX_UI_TEXT("가상 그림자 페이지"),
                     LUX_UI_TEXT("접촉 그림자"),
                     LUX_UI_TEXT("구름 그림자"),
                     LUX_UI_TEXT("ReSTIR 저장소"),
                     LUX_UI_TEXT("광선 노이즈"),
                     LUX_UI_TEXT("노이즈 제거 조명"),
                     LUX_UI_TEXT("화면 반사 신뢰도"),
                     LUX_UI_TEXT("볼류메트릭 안개"),
                     LUX_UI_TEXT("수심"),
                     LUX_UI_TEXT("구름 밀도"),
                     LUX_UI_TEXT("수면 반사"),
                     LUX_UI_TEXT("지형 재질 가중치"),
                     LUX_UI_TEXT("식생 LOD")});
    debug->setCurrentIndex(static_cast<int>(s.postDebugView));
    camera->addRow(LUX_UI_TEXT("진단 화면"), debug);
    Object::connect(debug, &ComboBox::currentIndexChanged, debug,
                    [&s](int i) { s.postDebugView = static_cast<LuxEngine::PostDebugView>(i); });
    auto* temporal = new ComboBox(&host);
    temporal->addItems({LUX_UI_TEXT("일반"), LUX_UI_TEXT("속도"), LUX_UI_TEXT("리액티브 마스크"),
                        LUX_UI_TEXT("히스토리 가중치"), LUX_UI_TEXT("히스토리 거부"),
                        LUX_UI_TEXT("투명 처리")});
    temporal->setCurrentIndex(static_cast<int>(s.temporalDebugView));
    camera->addRow(LUX_UI_TEXT("시간 누적 진단"), temporal);
    Object::connect(temporal, &ComboBox::currentIndexChanged, temporal, [&s, debug](int i) {
        s.temporalDebugView = static_cast<LuxEngine::TemporalDebugView>(i);
        if (i) {
            s.temporalAntialiasing = true;
            debug->setCurrentIndex(0);
        }
    });
    Object::connect(debug, &ComboBox::currentIndexChanged, debug, [temporal](int i) {
        if (i)
            temporal->setCurrentIndex(0);
    });
    Number(*camera, LUX_UI_TEXT("재질 밉 바이어스"), s.textureLodBias, -4, 4);
    Toggle(*camera, LUX_UI_TEXT("물리 카메라 노출"), s.physicalCamera.exposure);
    Number(*camera, LUX_UI_TEXT("ISO"), s.physicalCamera.iso, 1, 409600, 0);
    Number(*camera, LUX_UI_TEXT("셔터 시간 (초)"), s.physicalCamera.shutterSeconds, 1. / 65536., 60,
           6);
    Number(*camera, LUX_UI_TEXT("조리개 (f/)"), s.physicalCamera.fNumber, .5, 128);
    Number(*camera, LUX_UI_TEXT("HDR 단위당 휘도 (cd/m²)"), s.physicalCamera.luminanceScale, .00001,
           1e7, 5);
    Toggle(*camera, LUX_UI_TEXT("자동 노출"), s.autoExposure);
    auto& meter = s.metering;
    auto* low = Number(*camera, LUX_UI_TEXT("하위 계측 분위"), meter.lowPercentile, 0, .999);
    auto* high = Number(*camera, LUX_UI_TEXT("상위 계측 분위"), meter.highPercentile, .001, 1);
    low->setMaximum(high->value() - .001);
    high->setMinimum(low->value() + .001);
    Object::connect(low, &DoubleSpinBox::valueChanged, high,
                    [high](double x) { high->setMinimum(x + .001); });
    Object::connect(high, &DoubleSpinBox::valueChanged, low,
                    [low](double x) { low->setMaximum(x - .001); });
    auto* floor = Number(*camera, LUX_UI_TEXT("최소 자동 보정 (EV)"), meter.minEV, -20, 20);
    auto* ceiling = Number(*camera, LUX_UI_TEXT("최대 자동 보정 (EV)"), meter.maxEV, -20, 20);
    floor->setMaximum(ceiling->value());
    ceiling->setMinimum(floor->value());
    Object::connect(floor, &DoubleSpinBox::valueChanged, ceiling, &DoubleSpinBox::setMinimum);
    Object::connect(ceiling, &DoubleSpinBox::valueChanged, floor, &DoubleSpinBox::setMaximum);
    Number(*camera, LUX_UI_TEXT("밝아지는 속도 (1/s)"), meter.brightenSpeed, .01, 100);
    Number(*camera, LUX_UI_TEXT("어두워지는 속도 (1/s)"), meter.darkenSpeed, .01, 100);
    Number(*camera, LUX_UI_TEXT("노출 보정 (EV)"), meter.compensationEV, -16, 16);
    Number(*camera, LUX_UI_TEXT("지역 노출 강도"), s.localExposure, 0, 1);
    Toggle(*camera, LUX_UI_TEXT("물리 피사계 심도"), s.physicalCamera.depthOfField);
    Number(*camera, LUX_UI_TEXT("센서 세로 (mm)"), s.physicalCamera.sensorHeightMm, 1, 100);
    Number(*camera, LUX_UI_TEXT("초점 거리 (m)"), s.focusDistance, .001, 1e8);
    Number(*camera, LUX_UI_TEXT("자동 초점 속도 (1/s)"), s.physicalCamera.autofocusSpeed, 0, 100);
    auto* lens = group(LUX_UI_TEXT("블룸과 렌즈"));
    Toggle(*lens, LUX_UI_TEXT("물리 블룸"), s.physicallyBasedBloom);
    Number(*lens, LUX_UI_TEXT("블룸 강도"), s.bloomIntensity, 0, 2);
    Number(*lens, LUX_UI_TEXT("블룸 임계값"), s.bloomThreshold, 0, 65504);
    Number(*lens, LUX_UI_TEXT("블룸 반경"), s.bloomRadius, .25, 4);
    Number(*lens, LUX_UI_TEXT("블룸 산란"), s.bloomScatter, 0, 1);
    Number(*lens, LUX_UI_TEXT("고휘도 이상치 억제"), s.bloomFireflySuppression, 0, 1);
    Number(*lens, LUX_UI_TEXT("렌즈 플레어"), s.lensFlareStrength, 0, 2);
    Number(*lens, LUX_UI_TEXT("비네팅"), s.vignette, 0, 1);
    Number(*lens, LUX_UI_TEXT("필름 그레인"), s.filmGrain, 0, 1);
    Number(*lens, LUX_UI_TEXT("색수차 (1080p 픽셀)"), s.chromaticAberration, 0, 10);
    Number(*lens, LUX_UI_TEXT("렌즈 왜곡"), s.lensDistortion, -.35, .35);
    auto* grading = group(LUX_UI_TEXT("색 보정"));
    auto& grade = s.colorGrade;
    Number(*grading, LUX_UI_TEXT("화이트 밸런스 (K)"), grade.temperatureK, 2500, 25000, 0);
    Number(*grading, LUX_UI_TEXT("틴트"), grade.tint, -1, 1);
    Number(*grading, LUX_UI_TEXT("채도"), grade.saturation, 0, 2);
    Number(*grading, LUX_UI_TEXT("생동감"), grade.vibrance, -1, 1);
    Number(*grading, LUX_UI_TEXT("대비"), grade.contrast, .25, 4);
    Triplet(*grading, LUX_UI_TEXT("리프트"), grade.lift, -.5, .5);
    Triplet(*grading, LUX_UI_TEXT("감마"), grade.gamma, .1, 4);
    Triplet(*grading, LUX_UI_TEXT("게인"), grade.gain, 0, 4);
    Triplet(*grading, LUX_UI_TEXT("암부 (EV)"), grade.shadows, -4, 4);
    Triplet(*grading, LUX_UI_TEXT("중간톤 (EV)"), grade.midtones, -4, 4);
    Triplet(*grading, LUX_UI_TEXT("명부 (EV)"), grade.highlights, -4, 4);
    const auto lut = [&](const String& label, std::string& path) {
        auto* file = new LineEdit(String::fromUtf8(path), &host);
        grading->addRow(label, file);
        Object::connect(file, &LineEdit::textChanged, file,
                        [&path](const String& text) { path = text.toUtf8().toStdString(); });
    };
    lut(LUX_UI_TEXT("기본 LUT (.cube)"), s.colorGradingLut);
    lut(LUX_UI_TEXT("보조 LUT (.cube)"), s.secondaryColorGradingLut);
    Number(*grading, LUX_UI_TEXT("LUT 간 혼합"), s.colorGradingMix, 0, 1);
    Number(*grading, LUX_UI_TEXT("LUT 적용량"), s.colorGradingStrength, 0, 1);
    Volumes(*group(LUX_UI_TEXT("후처리 볼륨")), host, s.postVolumes);
}
} // namespace LuxEditor
