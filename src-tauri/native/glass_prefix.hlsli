// Ordered optical composition. Captured alpha and painted surface coverage
// remain separate; global tint enters the prefix once, before any glass.
// Derivation and source comparison: docs/LIQUID-GLASS-LAYERING.md
Texture2D<float> priorCoverage : register(t2);
Texture2D<float4> windowBase : register(t3);

struct PrefixResult { float4 color : SV_Target0; float coverage : SV_Target1; };

PrefixResult PrefixInitPS(Vertex input)
{
    float4 raw = sourceImage.Load(int3(input.position.xy, 0));
    PrefixResult result;
    result.color = float4(lerp(raw.rgb, tintRgba.rgb * raw.a, tintRgba.a), raw.a);
    result.coverage = 0;
    return result;
}

float3 PrefixSample(float2 pixel, float lod)
{
    float4 value = sourceImage.SampleLevel(linearClamp, clamp(pixel, 0.5, sceneSize - 0.5) / sceneSize, lod);
    return value.a > 0.00001 ? value.rgb / value.a : tintRgba.rgb;
}

PrefixResult PrefixGlassPS(Vertex input)
{
    float2 pixel = input.position.xy;
    PrefixResult result;
    result.color = sourceImage.Load(int3(pixel, 0));
    result.coverage = priorCoverage.Load(int3(pixel, 0));
    GlassControl control = controls[0];
    GlassBoundary boundary = GlassRoundedRect(pixel, control.rectXYWH, control.shape.x);
    if (boundary.distance >= 0.5) return result;
    float dpi = max(options.w, 0.01);
    float radius = clamp(control.shape.x, 0.0, 0.5 * min(control.rectXYWH.z, control.rectXYWH.w));
    bool reading = control.shape.z > 1.5;
    GlassOptics optics = GlassEvaluate(pixel, control.rectXYWH, control.shape.x,
        reading ? 0.0 : 26.0 * dpi, 1.52, control.shape.y);
    float lod = GlassSurfaceLod(control);
    float3 refracted = float3(PrefixSample(pixel + optics.offsetR, lod).r,
        PrefixSample(pixel + optics.offsetG, lod).g, PrefixSample(pixel + optics.offsetB, lod).b);
    float width = min(dpi, radius);
    float reflectionWeight = width > 0.0001 ? 1.0 - smoothstep(0.0, width, max(-boundary.distance, 0.0)) : 0.0;
    float facing = saturate(dot(boundary.outward, normalize(float2(0.55, 0.83))));
    float darkReflection = reflectionWeight * reflectionWeight * (0.035 + 0.17 * facing * facing);
    if (reading) darkReflection = 0.0;
    float f0 = (1.52 - 1.0) / (1.52 + 1.0); f0 *= f0;
    float reflection = 1.0 - (1.0 - f0) * (1.0 - optics.highlight);
    float3 glass = lerp(refracted * (1.0 - darkReflection), 1.0, reflection);
    result.color = lerp(result.color, float4(glass, 1), optics.coverage);
    result.coverage = lerp(result.coverage, 1.0, optics.coverage);
    return result;
}

PrefixResult PrefixTrackPS(Vertex input)
{
    float2 pixel = input.position.xy;
    PrefixResult result;
    result.color = sourceImage.Load(int3(pixel, 0));
    result.coverage = priorCoverage.Load(int3(pixel, 0));
    GlassTrack track = tracks[0];
    float coverage = saturate(0.5 - GlassRoundedRect(pixel, track.rectXYWH, track.shape.x).distance);
    float progress = track.shape.y <= 0.0 ? 0.0 : track.shape.y >= 1.0 ? 1.0
        : saturate(track.rectXYWH.x + track.rectXYWH.z * track.shape.y - pixel.x + 0.5);
    float4 premultiplied = lerp(float4(track.inactive.rgb * track.inactive.a, track.inactive.a),
        float4(track.active.rgb * track.active.a, track.active.a), progress) * coverage;
    result.color = premultiplied + (1.0 - premultiplied.a) * result.color;
    result.coverage = premultiplied.a + (1.0 - premultiplied.a) * result.coverage;
    return result;
}

float4 PrefixFinishPS(Vertex input) : SV_Target
{
    int3 pixel = int3(input.position.xy, 0);
    float3 current = sourceImage.Load(pixel).rgb;
    float3 initial = opticalBackdrop.Load(pixel).rgb;
    float coverage = priorCoverage.Load(pixel);
    return float4(current + (1.0 - coverage) * (windowBase.Load(pixel).rgb - initial), 1);
}
