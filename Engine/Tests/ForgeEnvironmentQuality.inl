void EnvironmentQuality() {
    std::printf("[environment_quality]\n");
    Session session(128, 128);
    if (!session.ok)
        return Check(false, "environment quality test device");
    Renderer3D::Context renderer(&session.gpu);
    PerspectiveCamera camera(ToRadians(60), 1, .1, 200);
    camera.SetPosition({0, 0, 3});
    camera.SetTarget({0, .8, 0});
    Render3DSettings settings;
    settings.imageBasedLighting = settings.shadows = false;
    settings.dynamicAtmosphere = true;
    auto capture = [&](GpuFrameCapture& out, const auto& submit) {
        return CaptureFrame(session, out, [&] {
            session.gpu.Clear(0, 0, 0, 1);
            renderer.BeginScene(camera, settings, 1);
            submit();
            renderer.EndScene();
            return renderer.GetLastAssetError().empty();
        });
    };
    GpuFrameCapture day, night, overcast;
    settings.sun.direction = {0, -1, 0};
    if (!capture(day, [] {}))
        return Check(false, "day sky captures");
    settings.sun.direction = {0, 1, 0};
    if (!capture(night, [] {}))
        return Check(false, "night sky captures");
    const auto noon = At(day, .5f, .25f), dark = At(night, .5f, .25f);
    Check(noon[2] > dark[2] + 25 && noon[2] > noon[0], "sky radiance follows solar elevation");
    settings.sun.direction = {0, -1, 0};
    const auto previousSun = settings.sun.intensity;
    settings.sun.intensity = 0;
    GpuFrameCapture unlitSky;
    if (!capture(unlitSky, [] {}))
        return Check(false, "unlit atmosphere captures");
    Check(At(unlitSky, .5f, .25f)[2] + 25 < noon[2], "atmospheric radiance follows sun power");
    settings.sun.intensity = previousSun;
    settings.cloudCoverage = 1;
    if (!capture(overcast, [] {}))
        return Check(false, "volumetric cloud layer captures");
    size_t changed = 0;
    for (size_t i = 0; i < day.rgba.size(); i += 4)
        changed += std::abs(int(day.rgba[i]) - int(overcast.rgba[i])) > 5;
    Check(changed > 256, "cloud extinction and inscattering change sky pixels");

    camera.SetTarget({0, 0, 0});
    settings.dynamicAtmosphere = false;
    settings.cloudCoverage = 0;
    settings.environmentColor = {0, 0, 0};
    settings.sun = {{0, 0, 1}, {1, 1, 1}, 4};
    Material3D leaf;
    leaf.alphaMode = MaterialAlphaMode::Opaque;
    const std::array<Vertex3D, 4> quad{{{{-.8, -.8, 0}, {0, 0, 1}, {}},
                                        {{.8, -.8, 0}, {0, 0, 1}, {}},
                                        {{.8, .8, 0}, {0, 0, 1}, {}, {1, 1, 1, 1}, 1},
                                        {{-.8, .8, 0}, {0, 0, 1}, {}, {1, 1, 1, 1}, 1}}};
    const std::array<uint32, 6> triangles{0, 1, 2, 0, 2, 3};
    Mesh3D mesh;
    if (!mesh.Upload(&session.gpu, quad, triangles))
        return Check(false, "test leaf upload");
    auto draw = [&] { renderer.DrawMesh(mesh, Mat4{}, leaf); };
    GpuFrameCapture opaque, transmitted, bent;
    if (!capture(opaque, draw))
        return Check(false, "opaque backlight capture");
    leaf.subsurfaceRadius = {.003, .0004, .0002};
    leaf.subsurfaceThickness = .001f;
    if (!capture(transmitted, draw))
        return Check(false, "thin sheet capture");
    const auto surface = At(transmitted, .5f, .5f);
    Check(surface[0] > At(opaque, .5f, .5f)[0] + 30 && surface[0] > surface[2] + 30,
          "wavelength-dependent subsurface transmission in a backlit sheet");
    settings.windVelocity = {8, 0};
    settings.weatherTime = 1.7;
    leaf.windAmplitude = .8f;
    if (!capture(bent, draw))
        return Check(false, "wind deformation capture");
    size_t upperChanges = 0, bottomChanges = 0;
    for (uint32 y = 0; y < bent.height; ++y)
        for (uint32 x = 0; x < bent.width; ++x) {
            const size_t i = 4 * (size_t(y) * bent.width + x);
            if (std::abs(int(bent.rgba[i]) - int(transmitted.rgba[i])) > 20) {
                upperChanges += y < bent.height / 2;
                bottomChanges += y >= bent.height / 2;
            }
        }
    std::printf("  Wind silhouette changed pixels at crown/root: %zu / %zu\n", upperChanges,
                bottomChanges);
    Check(upperChanges > 10 && upperChanges > bottomChanges, "wind bends tips more than roots");

    DecalProjector p;
    p.normal = {0, 0, 1};
    p.halfSize = {.23, .3, .05};
    std::vector<Vertex3D> projected;
    std::vector<uint32> indices;
    Check(AppendProjectedDecal(p, quad, triangles, projected, indices) && !indices.empty(),
          "a projector clips both receiver triangles");
    for (const auto& v : projected)
        Check(std::abs(v.position.x) <= .230001 && std::abs(v.position.y) <= .300001 &&
                  std::abs(v.position.z - p.normalOffset) < 1e-8 && v.texCoords.x >= -1e-8 &&
                  v.texCoords.x <= 1.000001,
              "decal remains on the receiver inside the UV rectangle");
    const auto vertexCount = projected.size();
    p.center.z = 1;
    Check(AppendProjectedDecal(p, quad, triangles, projected, indices) &&
              projected.size() == vertexCount,
          "decal projection stops at its depth boundary");
    Mesh3D decal;
    if (!decal.Upload(&session.gpu, projected, indices))
        return Check(false, "projected decal uploads");
    settings.sun.direction = {0, 0, -1};
    Material3D paint;
    paint.albedoColor = {.04f, .85f, .04f, 1};
    GpuFrameCapture painted;
    if (!capture(painted, [&] {
            renderer.DrawMesh(mesh, Mat4{}, {});
            renderer.DrawMesh(decal, Mat4{}, paint);
        }))
        return Check(false, "decal on wall captures");
    const auto center = At(painted, .5f, .5f), outside = At(painted, .66f, .5f);
    Check(center[1] > center[0] + 35 && std::abs(outside[1] - outside[0]) < 10,
          "projected paint appears only inside its footprint");
    camera.SetPosition({0, 2, 2});
    const auto horizontal = Mat4::RotateX(-ToRadians(90)) * Mat4::Scale({5, 5, 1});
    Material3D earth;
    earth.alphaMode = MaterialAlphaMode::Opaque;
    earth.albedoColor = {.27f, .19f, .13f, 1};
    earth.roughness = .88f;
    settings.environmentColor = {.7, .7, .7};
    settings.sun.intensity = 0;
    GpuFrameCapture dry, wet, protectedSurface;
    const auto drawEarth = [&] { renderer.DrawMesh(mesh, horizontal, earth); };
    earth.wettingResponse = 1;
    if (!capture(dry, drawEarth))
        return Check(false, "dry terrain captures");
    settings.surfaceWetness = 1;
    if (!capture(wet, drawEarth))
        return Check(false, "saturated terrain captures");
    earth.wettingResponse = 0;
    if (!capture(protectedSurface, drawEarth))
        return Check(false, "sheltered terrain captures");
    std::printf("  Soil (red channel) dry / wet / sheltered: %d / %d / %d\n", At(dry, .5f, .5f)[0],
                At(wet, .5f, .5f)[0], At(protectedSurface, .5f, .5f)[0]);
    Check(At(dry, .5f, .5f)[0] - At(wet, .5f, .5f)[0] > 8 && protectedSurface.rgba == dry.rgba,
          "rain darkens rough exposed soil while protected materials stay dry");

    auto water = WaterSurface::MakeMaterial();
    water.roughness = .11f;
    settings.sun = {{0, -.8, .6}, {1, 1, 1}, 3};
    settings.windVelocity = {6, 2};
    settings.weatherTime = 0;
    Vec3 shift{};
    const auto drawWater = [&] {
        renderer.DrawMesh(mesh, Mat4::Translate(shift + Vec3{0, -.9, 0}) * horizontal, earth);
        renderer.DrawMesh(mesh, Mat4::Translate(shift) * horizontal, water);
    };
    GpuFrameCapture smooth, ripples, later;
    if (!capture(smooth, drawWater))
        return Check(false, "smooth water captures");
    water.water->rippleStrength = 1;
    if (!capture(ripples, drawWater))
        return Check(false, "capillary detail captures");
    settings.weatherTime = .37;
    if (!capture(later, drawWater))
        return Check(false, "animated ripples capture");
    size_t normalChanges = 0, phaseChanges = 0;
    for (size_t i = 0; i < smooth.rgba.size(); i += 4) {
        normalChanges += std::abs(int(smooth.rgba[i]) - int(ripples.rgba[i])) > 3;
        phaseChanges += std::abs(int(later.rgba[i]) - int(ripples.rgba[i])) > 3;
    }
    Check(normalChanges > 120 && phaseChanges > 120,
          "wind-ripple slopes alter and animate reflected and refracted radiance");
    // Both spatial components repeat after 256 metres; a distant camera must preserve phase.
    shift = {268435456, 0, -536870912};
    camera.SetPosition(shift + Vec3{0, 2, 2});
    camera.SetTarget(shift);
    GpuFrameCapture distant;
    if (!capture(distant, drawWater))
        return Check(false, "rebased water captures");
    size_t different = 0;
    int largestError = 0;
    for (size_t i = 0; i < distant.rgba.size(); ++i) {
        const int delta = std::abs(int(distant.rgba[i]) - int(later.rgba[i]));
        different += delta > 1;
        largestError = std::max(largestError, delta);
    }
    std::printf("  Rebased ripple channels differing by more than one code: %zu / %zu, max %d\n",
                different, distant.rgba.size(), largestError);
    Check(different == 0, "water detail is stable after a large-world rebase");
    Check(session.gpu.GetCounters().validationErrors == 0, "environment rendering validates");
}
