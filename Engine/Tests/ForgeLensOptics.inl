// Measure each lens effect with the other camera effects disabled. A neutral sensor card
// covers the whole frame, with a single optional off-axis, unlit HDR lamp.
void LensOptics() {
    std::printf("[lens_optics]\n");
    constexpr uint32 width = 384, height = 216;
    Session session(width, height);
    if (!session.ok)
        return Check(false, "lens optics device initializes");
    Renderer3D::Context renderer(&session.gpu);
    PerspectiveCamera camera(ToRadians(60), double(width) / height, .1, 50);
    camera.SetPosition({0, 0, 3});
    camera.SetTarget({0, 0, 0});
    Render3DSettings controls;
    controls.shadows = controls.imageBasedLighting = controls.toneMapping = false;
    controls.sun.intensity = controls.environmentIntensity = 0;
    Material3D card, lamp;
    card.alphaMode = lamp.alphaMode = MaterialAlphaMode::Opaque;
    card.emissive = {.18, .18, .18};
    lamp.emissive = {12, 12, 12};
    Render3DSettings settings = controls;
    auto capture = [&](GpuFrameCapture& frame, bool drawLamp) {
        return CaptureFrame(session, frame, [&] {
            session.gpu.Clear(0, 0, 0, 1);
            renderer.BeginScene(camera, settings, 1);
            renderer.DrawCube({0, 0, -1}, {22, 22, .02}, card);
            if (drawLamp)
                renderer.DrawCube({1.85, .55, 0}, {.38, .34, .02}, lamp);
            renderer.EndScene();
            return renderer.GetLastAssetError().empty();
        });
    };

    GpuFrameCapture base, result, next;
    if (!capture(base, false))
        return Check(false, "neutral lens reference captures");
    settings.vignette = 1;
    if (!capture(result, false))
        return Check(false, "vignette-only capture");
    Check(std::abs(At(result, .5f, .5f)[0] - At(base, .5f, .5f)[0]) <= 1 &&
              At(result, .025f, .025f)[0] < At(base, .025f, .025f)[0] - 30,
          "vignette lowers the corner exposure while preserving the optical center");

    settings = controls;
    settings.filmGrain = 1;
    if (!capture(result, false) || !capture(next, false))
        return Check(false, "successive grain-only frames capture");
    double signedNoise = 0, absoluteNoise = 0, temporalChange = 0;
    for (size_t i = 0; i < base.rgba.size(); i += 4) {
        const int delta = int(result.rgba[i]) - base.rgba[i];
        signedNoise += delta;
        absoluteNoise += std::abs(delta);
        temporalChange += std::abs(int(next.rgba[i]) - result.rgba[i]);
    }
    const double pixels = base.rgba.size() / 4;
    Check(std::abs(signedNoise / pixels) < 1 && absoluteNoise / pixels > 3 &&
              temporalChange / pixels > 3,
          "film grain varies across pixels and frames while retaining mean exposure");

    settings = controls;
    if (!capture(base, true))
        return Check(false, "off-axis lamp reference captures");
    settings.chromaticAberration = 10;
    if (!capture(result, true))
        return Check(false, "chromatic aberration alone captures");
    size_t fringes = 0, greenChanges = 0;
    for (size_t i = 0; i < base.rgba.size(); i += 4) {
        fringes += std::abs(int(result.rgba[i]) - result.rgba[i + 2]) > 12;
        greenChanges += std::abs(int(result.rgba[i + 1]) - base.rgba[i + 1]) > 2;
    }
    Check(fringes > 20 && greenChanges == 0,
          "lateral aberration splits red and blue at the neutral lamp edge and preserves green");

    const auto lampCenterX = [](const GpuFrameCapture& frame) {
        double xSum = 0;
        size_t count = 0;
        for (uint32 y = 0; y < frame.height; ++y)
            for (uint32 x = 0; x < frame.width; ++x)
                if (Pixel(frame, x, y)[0] > 220) {
                    xSum += x;
                    ++count;
                }
        return count ? xSum / double(count) : 0.0;
    };
    settings = controls;
    settings.lensDistortion = .28f;
    if (!capture(result, true))
        return Check(false, "radial lens distortion alone captures");
    Check(lampCenterX(result) > lampCenterX(base) + 8 && At(result, .5f, .5f) == At(base, .5f, .5f),
          "radial distortion moves the off-axis lamp while preserving the center");

    settings = controls;
    settings.lensFlareStrength = 1;
    if (!capture(result, true))
        return Check(false, "lens flare alone captures");
    const auto ghost = At(result, .3f, .605f);
    Check(ghost[0] > At(base, .3f, .605f)[0] + 20 && ghost[0] > ghost[2] + 5,
          "off-axis HDR lamp creates a tinted lens ghost across the optical center");
    settings.bloomThreshold = 65504;
    if (!capture(next, true))
        return Check(false, "flare with disabled bloom captures");
    Check(At(next, .3f, .605f) == ghost,
          "standalone flare radiance is independent of the inactive bloom threshold");
    Check(session.gpu.GetCounters().validationErrors == 0, "camera optics GPU barriers validate");
}
