#include "glass_optics.hlsli"

struct GlassControl
{
    float4 rectXYWH;
    float4 shape; // Radius in pixels, pressure [0, 1], material (0 clear / 1 regular / 2 reading), reserved.
};

struct GlassTrack
{
    float4 rectXYWH;
    float4 shape; // Radius, progress, owning control index, reserved.
    float4 active;
    float4 inactive;
};

// Keep this layout identical to the native renderer's constant buffer:
// 0: sceneSize + blurDirection, 16: tintRgba, 32: options,
// 48: controls (64 * 32 bytes), 2096: track metadata,
// 2112: tracks (64 * 64 bytes). Total: 6208 bytes.
cbuffer Parameters : register(b0)
{
    float2 sceneSize;
    float2 blurDirection;
    float4 tintRgba;
    float4 options; // Window blur, count, apply tint / layered optics, DPI scale.
    GlassControl controls[64];
    float4 trackMeta; // Packed track count, reserved, reserved, reserved.
    GlassTrack tracks[64];
};

// CPU-precomputed uniform kernel. Direct shader fixtures can leave b1 unbound
// and exercise the analytical path below as an independent numerical oracle.
cbuffer GaussianKernel : register(b1)
{
    float4 gaussianMeta; // Pair count, center weight, matching physical sigma.
    float4 gaussianPairs[192]; // Bilinear offset, normalized pair weight.
};

Texture2D<float4> sourceImage : register(t0);
// The original premultiplied backdrop and its mip chain remain separate from
// the window's blurred/tinted base. Otherwise the base destroys fine detail
// before the curved surface ever has a chance to refract it.
Texture2D<float4> opticalBackdrop : register(t1);
SamplerState linearClamp : register(s0);

struct Vertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex VSMain(uint id : SV_VertexID)
{
    Vertex output;
    // One full-screen triangle; source UV (0, 0) is the top-left pixel.
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0)
        + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 BlurPS(Vertex input) : SV_Target
{
    float sigma = clamp(options.x, 0.0, 128.0);
    float4 blurred;
    if (sigma <= 0.001)
    {
        blurred = sourceImage.SampleLevel(linearClamp, input.uv, 0.0);
    }
    else if (sigma > 4.0 && gaussianMeta.z == sigma && gaussianMeta.x > 0.0)
    {
        float4 accumulated = sourceImage.SampleLevel(linearClamp, input.uv, 0.0) * gaussianMeta.y;
        int pairs = int(min(gaussianMeta.x, 192.0));
        [loop]
        for (int tap = 0; tap < pairs; ++tap)
        {
            float2 delta = blurDirection * gaussianPairs[tap].x / sceneSize;
            accumulated += (sourceImage.SampleLevel(linearClamp, input.uv + delta, 0.0)
                + sourceImage.SampleLevel(linearClamp, input.uv - delta, 0.0)) * gaussianPairs[tap].y;
        }
        blurred = accumulated;
    }
    else if (sigma > 4.0)
    {
        // Sparse taps alias fine patterns once their spacing exceeds a pixel.
        // Pair adjacent discrete Gaussian weights into one bilinear sample.
        int radius = int(ceil(3.0 * sigma));
        float4 accumulated = sourceImage.SampleLevel(linearClamp, input.uv, 0.0);
        float weightSum = 1.0;
        [loop]
        for (int tap = 1; tap <= radius; tap += 2)
        {
            float d1 = float(tap) / sigma;
            float d2 = float(tap + 1) / sigma;
            float w1 = exp(-0.5 * d1 * d1);
            float w2 = tap + 1 <= radius ? exp(-0.5 * d2 * d2) : 0.0;
            float pairWeight = w1 + w2;
            float offset = float(tap) + w2 / pairWeight;
            float2 delta = blurDirection * offset / sceneSize;
            accumulated += (sourceImage.SampleLevel(linearClamp, input.uv + delta, 0.0)
                + sourceImage.SampleLevel(linearClamp, input.uv - delta, 0.0)) * pairWeight;
            weightSum += 2.0 * pairWeight;
        }
        blurred = accumulated / weightSum;
    }
    else
    {
        // Keep the existing closely spaced taps for sigma up to four pixels.
        // WGC supplies premultiplied pixels, including zero coverage outside
        // the rounded window. Carry color and coverage through both passes;
        // treating uncovered black as opaque would darken the visible edge.
        float2 stepUv = blurDirection * (sigma * 0.25) / sceneSize;
        float4 accumulated = 0.0;
        float weightSum = 0.0;
        [unroll]
        for (int tap = -12; tap <= 12; ++tap)
        {
            float normalizedDistance = float(tap) * 0.25;
            float weight = exp(-0.5 * normalizedDistance * normalizedDistance);
            accumulated += sourceImage.SampleLevel(linearClamp,
                input.uv + float(tap) * stepUv, 0.0) * weight;
            weightSum += weight;
        }
        blurred = accumulated / weightSum;
    }

    // The horizontal target retains premultiplied RGB and weighted coverage.
    // Normalize color only after the vertical pass, then apply the window tint.
    if (options.z <= 0.5)
        return blurred;
    float3 color = blurred.a > 0.00001 ? blurred.rgb / blurred.a : tintRgba.rgb;
    color = lerp(color, tintRgba.rgb, saturate(tintRgba.a));
    return float4(color, 1.0);
}

float3 CompositeTracks(float2 pixel, float3 background)
{
    int count = int(clamp(trackMeta.x, 0.0, 64.0));
    // Tracks share the controls' paint order and enter the sampled source once,
    // before refraction. The packed count avoids scanning unused control slots.
    [loop]
    for (int index = 0; index < count; ++index)
    {
        float4 rect = tracks[index].rectXYWH;
        if (rect.z <= 0.0 || rect.w <= 0.0
            || any(pixel < rect.xy - 0.5)
            || any(pixel > rect.xy + rect.zw + 0.5))
            continue;
        GlassTrack track = tracks[index];
        float coverage = saturate(0.5 - GlassRoundedRect(pixel, rect, track.shape.x).distance);
        if (coverage <= 0.0) continue;
        // Match the input's straight progress cutoff. Composite each tint over
        // the source before interpolating coverage, without multiplying alphas.
        float progressCoverage = track.shape.y <= 0.0 ? 0.0 : track.shape.y >= 1.0 ? 1.0
            : saturate(rect.x + rect.z * track.shape.y - pixel.x + 0.5);
        float3 inactive = lerp(background, track.inactive.rgb, track.inactive.a);
        float3 active = lerp(background, track.active.rgb, track.active.a);
        background = lerp(background, lerp(inactive, active, progressCoverage), coverage);
    }
    return background;
}

float3 SampleBackground(float2 pixel)
{
    pixel = clamp(pixel, 0.5, sceneSize - 0.5);
    return CompositeTracks(pixel, sourceImage.SampleLevel(linearClamp, pixel / sceneSize, 0.0).rgb);
}

float3 SampleGlass(float2 pixel, float lod, float tint)
{
    pixel = clamp(pixel, 0.5, sceneSize - 0.5);
    float4 sampled = opticalBackdrop.SampleLevel(linearClamp, pixel / sceneSize, lod);
    // Each mip carries premultiplied color and coverage. Normalize only after
    // sampling; transparent rounded-window pixels must never add a black rim.
    float3 color = sampled.a > 0.00001 ? sampled.rgb / sampled.a : tintRgba.rgb;
    color = lerp(color, tintRgba.rgb, saturate(tint));
    return CompositeTracks(pixel, color);
}

float GlassSurfaceLod(GlassControl control)
{
    float dpi = max(options.w, 0.01);
    float density = saturate((options.x / dpi - 1.5) / 14.5);
    // Clear optics retain fine source detail, with a uniform filtering footprint
    // adding up to one CSS pixel across the global clarity range. Reading
    // planes explicitly choose regular glass and one uniform scattering radius.
    float clearFootprint = 1.0 + density * dpi;
    float regularFootprint = sqrt(12.0) * max(2.0 * dpi, options.x);
    float readingFootprint = sqrt(12.0) * max(4.0 * dpi, options.x);
    float footprint = control.shape.z > 1.5 ? readingFootprint
        : control.shape.z > 0.5 ? regularFootprint : clearFootprint;
    return log2(max(1.0, footprint));
}

float4 OpticsPS(Vertex input) : SV_Target
{
    float2 pixel = input.position.xy;
    float3 background = SampleBackground(pixel);
    float dpi = max(options.w, 0.01);
    int count = int(clamp(options.y, 0.0, 64.0));
    int selected = -1;

    // Controls arrive in paint order. Resolve the topmost shape before optical
    // evaluation so overlaps never accumulate several rims or refractions.
    // The foreground owns one persistent CSS outer shadow across renderer
    // handoffs; the sampled frame contains only the optical surface.
    [loop]
    for (int index = count - 1; index >= 0; --index)
    {
        float4 rect = controls[index].rectXYWH;
        if (rect.z <= 0.0 || rect.w <= 0.0
            || any(pixel < rect.xy - 0.5)
            || any(pixel > rect.xy + rect.zw + 0.5))
            continue;

        if (GlassRoundedRect(pixel, rect, controls[index].shape.x).distance < 0.5)
        {
            selected = index;
            break;
        }
    }

    if (selected < 0)
        return float4(background, 1.0);

    GlassControl control = controls[selected];
    const float glassIor = 1.52;
    bool reading = control.shape.z > 1.5;
    GlassOptics optics = GlassEvaluate(pixel, control.rectXYWH, control.shape.x,
        reading ? 0.0 : 26.0 * dpi, glassIor, control.shape.y);
    GlassBoundary boundary = GlassRoundedRect(pixel, control.rectXYWH, control.shape.x);
    float radius = clamp(control.shape.x, 0.0, 0.5 * min(control.rectXYWH.z, control.rectXYWH.w));
    float3 refracted;
    if (options.z > 0.5)
    {
        // Clarity belongs to the whole surface. Curvature changes ray offsets
        // and reflection only; it never switches a clear rim into a frosted core.
        float lod = GlassSurfaceLod(control);
        // Keep uniform backgrounds uniform. A lower tint around the entire
        // shoulder creates a thick dark plastic bevel even without detail.
        // Separation comes from source-dependent optics and elevation.
        refracted.r = SampleGlass(pixel + optics.offsetR, lod, tintRgba.a).r;
        refracted.g = SampleGlass(pixel + optics.offsetG, lod, tintRgba.a).g;
        refracted.b = SampleGlass(pixel + optics.offsetB, lod, tintRgba.a).b;
    }
    else
    {
        // Isolated optical evaluation retains its input unchanged. Production
        // always enables layered optics; this path also permits direct math QA.
        refracted.r = SampleBackground(pixel + optics.offsetR).r;
        refracted.g = SampleBackground(pixel + optics.offsetG).g;
        refracted.b = SampleBackground(pixel + optics.offsetB).b;
    }

    float reflectionWidth = min(1.0 * dpi, radius);
    float reflectionWeight = reflectionWidth > 0.0001
        ? 1.0 - smoothstep(0.0, reflectionWidth, max(-boundary.distance, 0.0))
        : 0.0;
    float darkFacing = saturate(dot(boundary.outward, normalize(float2(0.55, 0.83))));
    // A neutral, darker environment return on the lower/right shoulder gives
    // a pale background a visible curved edge. Directional reflections vanish
    // inward before the SDF's medial axis.
    float darkReflection = reflectionWeight * reflectionWeight
        * (0.035 + 0.17 * darkFacing * darkFacing);
    if (reading) darkReflection = 0.0;
    // The flat face still reflects a weak neutral environment. Use normal-
    // incidence Fresnel reflectance, not a second outline or depth tint ring.
    // Combine reflectances before coverage so the shoulder is not double lit.
    float f0 = (glassIor - 1.0) / (glassIor + 1.0);
    f0 *= f0;
    float surfaceReflection = 1.0 - (1.0 - f0) * (1.0 - optics.highlight);
    float3 glass = lerp(refracted * (1.0 - darkReflection),
        float3(1.0, 1.0, 1.0), surfaceReflection);
    // Apply silhouette coverage once. The source contains no control decoration
    // or foreground, and the final composited surface stays opaque.
    return float4(lerp(background, glass, optics.coverage), 1.0);
}

#include "glass_prefix.hlsli"
