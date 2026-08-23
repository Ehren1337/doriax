#version 450

// Premultiplied-alpha Gaussian blur of the SSR result, used to fake glossy (rough)
// reflections. The SSR pass renders with alpha blending into a zero-cleared target,
// so its output is already premultiplied (rgb = reflColor * mask); this blur must
// therefore accumulate rgb directly (NOT rgb*a, which would multiply the mask in a
// second time and darken partial-coverage reflections). Unhit (mask=0) neighbours
// contribute nothing, so no black bleed.
//
// The tap offsets are SCALED by the radius (a fixed circular tap grid spread over
// +/- radius pixels), so the blur ramps smoothly and continuously with the radius
// instead of changing only when the radius crosses integer pixel boundaries.

#ifndef SSR_BLUR_TAPS
#define SSR_BLUR_TAPS 6   // taps per side; kernel is (2*TAPS+1)^2 over a disc
#endif

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_ssrTexture;
uniform sampler u_ssr_smp;
uniform texture2D u_gbufferTexture;
uniform sampler u_gbuffer_smp;

uniform u_fs_ssrBlurParams {
    vec4 params; // xy = 1/textureSize, z = max radius (pixels), w = flip G-buffer Y
} blur;

void main(){
    vec2 texel = blur.params.xy;

    // Per-pixel roughness drives the blur amount: a mirror (roughness 0) stays sharp,
    // a rough surface blurs out to the full radius. The G-buffer is in depth space,
    // so sample it with the flip this composite-space pass was given.
    vec2 g = v_texcoord;
    if (blur.params.w > 0.5) g.y = 1.0 - g.y;
    float roughness = texture(sampler2D(u_gbufferTexture, u_gbuffer_smp), g).b;
    float radius = blur.params.z * roughness;

    // negligible radius => passthrough (keeps the sharp path exact for smooth surfaces)
    if (radius < 0.25){
        frag_color = texture(sampler2D(u_ssrTexture, u_ssr_smp), v_texcoord);
        return;
    }

    vec3 sumColor = vec3(0.0); // premultiplied reflection color
    float sumMask = 0.0;
    float sumWeight = 0.0;

    float invTaps = 1.0 / float(SSR_BLUR_TAPS);
    for (int j = -SSR_BLUR_TAPS; j <= SSR_BLUR_TAPS; ++j){
        for (int i = -SSR_BLUR_TAPS; i <= SSR_BLUR_TAPS; ++i){
            vec2 unit = vec2(float(i), float(j)) * invTaps; // normalized [-1,1]
            float r2 = dot(unit, unit);
            if (r2 > 1.0) continue; // circular kernel

            float w = exp(-r2 * 2.0); // gaussian over the normalized disc
            vec2 offset = unit * radius * texel; // spread scales with radius
            // Explicit LOD prevents D3D11 from unrolling the blur loops.
            vec4 s = textureLod(sampler2D(u_ssrTexture, u_ssr_smp), v_texcoord + offset, 0.0);

            sumColor += s.rgb * w; // s.rgb is already premultiplied (mask baked in)
            sumMask  += s.a * w;
            sumWeight += w;
        }
    }

    // Output straight color (= premultiplied / alpha); the pass's alpha blend then
    // re-premultiplies it, matching the SSR pass and keeping the composite consistent.
    vec3 outColor = (sumMask > 0.0001) ? (sumColor / sumMask) : vec3(0.0);
    float outMask = (sumWeight > 0.0001) ? (sumMask / sumWeight) : 0.0;

    frag_color = vec4(outColor, outMask);
}
