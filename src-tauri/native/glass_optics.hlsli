#ifndef TIEZ_GLASS_OPTICS_HLSLI
#define TIEZ_GLASS_OPTICS_HLSLI

// All positions, rectangle dimensions, radii and thicknesses use source pixels.
// This is an original UI lens approximation, not Apple's implementation.
// Geometry-driven light and depth: https://developer.apple.com/videos/play/wwdc2025/219/
// Separate background sampling: https://aave.com/design/building-glass-for-the-web
// Snell / wavelength-dependent IOR: https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission
// Sample undecorated background at pixel + offsetR/G/B; apply tint, this one
// highlight and foreground afterward. Composite coverage exactly once, using
// one source alpha (normally the green sample), never the sum of RGB alphas.

struct GlassBoundary
{
    float distance;
    float2 outward;
};

struct GlassOptics
{
    float coverage;
    float3 normal;
    float thickness;
    float2 offsetR;
    float2 offsetG;
    float2 offsetB;
    float highlight; // Unpremultiplied; the caller applies coverage once.
};

GlassBoundary GlassRoundedRect(float2 pixel, float4 rectXYWH, float radiusPx)
{
    float2 halfSize = max(rectXYWH.zw, 0.0) * 0.5;
    float radius = clamp(radiusPx, 0.0, min(halfSize.x, halfSize.y));
    float2 local = pixel - rectXYWH.xy - halfSize;
    float2 q = abs(local) - halfSize + radius;
    float2 corner = max(q, 0.0);
    float cornerLength = length(corner);
    GlassBoundary boundary;
    boundary.distance = cornerLength + min(max(q.x, q.y), 0.0) - radius;
    boundary.outward = cornerLength > 0.00001
        ? corner / cornerLength
        : (q.x > q.y ? float2(1.0, 0.0) : float2(0.0, 1.0));
    boundary.outward *= sign(local);
    return boundary;
}

float2 GlassSnellOffset(float3 normal, float heightPx, float ior)
{
    if (ior <= 1.0 || heightPx <= 0.0) return 0.0;
    // Air -> curved glass surface -> background plane at the flat underside.
    // One interface and a planar receiver, not a general volumetric ray tracer.
    float3 ray = refract(float3(0.0, 0.0, -1.0), normal, 1.0 / ior);
    return ray.xy * (heightPx / max(-ray.z, 0.0001));
}

// A parallel slab under a virtual perspective view has a lateral exit shift
// even where its faces are flat. Subtract the un-refracted receiver position.
float2 GlassBodyOffset(float2 local, float focalDistance, float height, float ior)
{
    if (ior <= 1.0 || height <= 0.0) return 0.0;
    float3 incident = normalize(float3(local, -focalDistance));
    float3 inside = refract(incident, float3(0.0, 0.0, 1.0), 1.0 / ior);
    return height * (inside.xy / max(-inside.z, 0.0001) - incident.xy / max(-incident.z, 0.0001));
}

GlassOptics GlassEvaluate(float2 pixel, float4 rectXYWH, float radiusPx,
    float thicknessPx, float ior, float interaction)
{
    GlassOptics optics = (GlassOptics)0;
    optics.normal = float3(0.0, 0.0, 1.0);
    if (rectXYWH.z <= 0.0 || rectXYWH.w <= 0.0) return optics;

    float radius = clamp(radiusPx, 0.0, 0.5 * min(rectXYWH.z, rectXYWH.w));
    GlassBoundary boundary = GlassRoundedRect(pixel, rectXYWH, radius);
    // A one-source-pixel antialias transition. No screen-space derivative or
    // texture dependency, so this helper also works in compute shaders.
    optics.coverage = saturate(0.5 - boundary.distance);
    if (optics.coverage <= 0.0) return optics;

    float press = smoothstep(0.0, 1.0, saturate(interaction));
    float requestedHeight = max(thicknessPx, 0.0) * (1.0 + 0.12 * press);
    float shoulder = min(radius, requestedHeight * 1.5);
    if (shoulder <= 0.0001) return optics;

    // Limit the slab aspect ratio to keep the inverse image mapping from
    // folding into a duplicated contour even at high IOR or capsule centers.
    // Let the bounded slab grow with pressure as well. A fixed cap erased the
    // entire thickness response of the usual small-radius controls.
    float height = min(requestedHeight, shoulder * 0.75 * (1.0 + 0.12 * press));
    float depth = max(-boundary.distance, 0.0);
    float t = saturate(depth / shoulder);
    float remaining = 1.0 - t;
    optics.thickness = height * (1.0 - remaining * remaining * remaining);
    float slope = (3.0 * height / shoulder) * remaining * remaining;
    optics.normal = normalize(float3(boundary.outward * slope, 1.0));

    float glassIor = clamp(ior, 1.0, 2.5);
    float dispersion = min(0.16, 0.28 * (glassIor - 1.0));
    // Body direction is continuous through the SDF medial axis. Its window
    // becomes constant before the boundary normal can switch branches.
    float bodyWindow = smoothstep(0.0, 1.0, t);
    float2 local = pixel - rectXYWH.xy - 0.5 * rectXYWH.zw;
    // All lengths scale with DPI. Keep the virtual camera at the resting
    // distance so pressing the slab still increases its optical depth.
    float restingShoulder = min(radius, max(thicknessPx, 0.0) * 1.5);
    float restingHeight = min(max(thicknessPx, 0.0), restingShoulder * 0.75);
    float focalDistance = max(6.0 * restingHeight, 0.3 * max(rectXYWH.z, rectXYWH.w));
    optics.offsetR = bodyWindow * GlassBodyOffset(local, focalDistance, height, glassIor - dispersion);
    optics.offsetG = bodyWindow * GlassBodyOffset(local, focalDistance, height, glassIor);
    optics.offsetB = bodyWindow * GlassBodyOffset(local, focalDistance, height, glassIor + dispersion);

    // Shoulder slope and highlight vanish here; the smooth body shift remains.
    if (t >= 1.0) return optics;

    // Project onto a fixed receiver plane instead of multiplying by the local
    // height, which approaches zero exactly where the surface bends most.
    // Keep the smooth cubic normal: it flattens before the SDF medial axis.
    // Higher IOR reduces receiver depth to keep the inverse map monotone.
    // This UI approximation is also used by anuero/LiquidGlass/Shaders.metal;
    // it is not a claim about Apple's private optical implementation.
    float projectionHeight = height / (1.0 + 0.5 * max(glassIor - 1.52, 0.0));
    // The existing shoulder and the broad body shift form one sampling field.
    // Foreground text is drawn later and never enters this optical input.
    optics.offsetR += GlassSnellOffset(optics.normal, projectionHeight, glassIor - dispersion);
    optics.offsetG += GlassSnellOffset(optics.normal, projectionHeight, glassIor);
    optics.offsetB += GlassSnellOffset(optics.normal, projectionHeight, glassIor + dispersion);

    // A single broad environment strip, connected to the silhouette. Its
    // support and intensity decrease inward, preventing isolated bright blobs.
    float2 light = normalize(float2(-0.55 + 0.10 * press, -0.83 - 0.05 * press));
    float facing = saturate(dot(boundary.outward, light));
    float f0 = (glassIor - 1.0) / (glassIor + 1.0);
    f0 *= f0;
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - optics.normal.z, 5.0);
    float edgeWeight = remaining * remaining * remaining * remaining;
    optics.highlight = glassIor > 1.0
        ? saturate(3.0 * fresnel * edgeWeight * facing * facing * facing)
        : 0.0;
    return optics;
}

#endif
