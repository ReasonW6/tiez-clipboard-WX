#ifndef TIEZ_GLASS_PROBE_LAYOUT_HLSLI
#define TIEZ_GLASS_PROBE_LAYOUT_HLSLI
#include "glass_optics.hlsli"

// Diagnostic geometry only. Optical math belongs exclusively to glass_optics.hlsli.
GlassOptics GlassProbeEvaluate(float2 pixel, float2 outputSize, float controlsLayout)
{
    const float4 shapes[7] = {
        float4(24.0, 24.0, 100.0, 100.0),
        float4(144.0, 24.0, 112.0, 112.0),
        float4(276.0, 24.0, 104.0, 104.0),
        float4(24.0, 158.0, 232.0, 84.0),
        float4(282.0, 158.0, 98.0, 284.0),
        float4(24.0, 270.0, 112.0, 112.0),
        float4(156.0, 284.0, 100.0, 100.0)
    };
    const float radii[7] = {50.0, 28.0, 52.0, 42.0, 49.0, 28.0, 50.0};
    float scale = min(outputSize.x / 420.0, outputSize.y / 490.0);
    float2 origin = (outputSize - float2(420.0, 490.0) * scale) * 0.5;
    float4 selectedRect = float4(12.0, 12.0, outputSize - 24.0);
    float selectedRadius = 36.0;
    float selectedThickness = 26.0;
    if (controlsLayout > 0.5) {
        selectedRect = 0.0;
        selectedRadius = 0.0;
        selectedThickness *= scale;
    }
    [unroll]
    for (int index = 0; index < 7; ++index) {
        float4 rect = float4(origin + shapes[index].xy * scale, shapes[index].zw * scale);
        float radius = radii[index] * scale;
        if (controlsLayout > 0.5 && GlassRoundedRect(pixel, rect, radius).distance < 0.5) {
            selectedRect = rect;
            selectedRadius = radius;
        }
    }
    return GlassEvaluate(pixel, selectedRect, selectedRadius, selectedThickness, 1.52, 0.0);
}
#endif
