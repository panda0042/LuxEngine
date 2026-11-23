#ifndef LUX_RAY_VISIBILITY_H
#define LUX_RAY_VISIBILITY_H

// The caller supplies an already biased origin and a normalized direction.
bool RayOccluded(float3 origin, float3 direction, float maxDistance) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001f;
    ray.TMax = maxDistance;
    // Only opaque instances block sunlight. Water uses a separate instance mask.
    RayQueryAnyHit(gRayScene, RAY_TRAVERSAL_FLAG_DEFAULT, RAY_FLAG_FORCE_OPAQUE, ray, 0x1u, hit);
    RayQueryBeginForEachCandidate(hit) {
        RayQueryProceed(hit);
    }
    RayQueryEndForEachCandidate(hit) return RayQueryIsHitTriangle(hit);
}

#endif
