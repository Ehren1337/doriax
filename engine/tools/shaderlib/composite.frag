#version 450

// Composites the offscreen scene color with the screen-space reflection buffer
// and writes the result to the real render target.
//
// Energy-conserving SSR-over-IBL (Unity/Godot model): the lit scene color already
// contains the IBL specular (environment reflection). Adding SSR on top would
// double-count it. So for surfaces that use IBL, this pass RECOMPUTES the IBL
// specular term from the G-buffer (normal/roughness/metallic/albedo) + the
// prefiltered environment, then REPLACES it with the screen-space reflection
// weighted by the SSR confidence: where SSR has a hit it takes over, and where it
// misses the IBL reflection remains. Non-IBL surfaces keep the simple additive path.
// Local probes also use that additive fallback: a single fullscreen composite cannot
// reconstruct multiple per-object cubemap bindings without a deferred probe atlas.
//
// A fullscreen pass fragment at v_texcoord t writes destination texel t and samples
// scene/SSR (composite space) at t. The G-buffer is in depth space, sampled at
// the flipped coordinate when params.y says the two spaces differ.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_ssrTexture;
uniform sampler u_ssr_smp;
uniform texture2D u_depthTexture;     // G-buffer color[0]: packed depth
uniform sampler u_depth_smp;
uniform texture2D u_gbufferTexture;   // G-buffer color[1]: oct normal + roughness + metallic
uniform sampler u_gbuffer_smp;
uniform texture2D u_albedoTexture;    // G-buffer color[2]: base color + hasIBL
uniform sampler u_albedo_smp;
uniform textureCube u_GGXEnvTexture;  // prefiltered environment (GGX)
uniform sampler u_GGXEnv_smp;

uniform u_fs_compositeParams {
    mat4 invProjection;
    mat4 invView;
    vec4 params;    // x = intensity, y = flip G-buffer Y, z = debug, w = unused
    vec4 envColor;  // rgb = env color (linear), w = env rotation (radians)
} comp;

#include "includes/depth_util.glsl"
#include "includes/octahedral.glsl"
#include "includes/srgb.glsl"

// Must match IBLEnvironment::SPECULAR_MIPS - 1 and ibl.glsl's ENV_GGX_LODS.
const float ENV_GGX_LODS = 7.0;

// analytic GGX environment BRDF approximation (copy of ibl.glsl envBRDFApprox)
vec2 envBRDFApprox(float NdotV, float roughness){
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

vec3 rotateEnv(vec3 dir, float angle){
    float c = cos(angle);
    float s = sin(angle);
    return vec3(c * dir.x - s * dir.z, dir.y, s * dir.x + c * dir.z);
}

// See ssr.frag: the packed depth is backend-independent, so no depth-range branch,
// but Y still needs the per-backend mapping.
vec3 reconstructViewPos(vec2 uv, float depth01){
    vec3 ndc = vec3(uv.x * 2.0 - 1.0, depthUVToNDCY(uv.y), depth01 * 2.0 - 1.0);
    vec4 view = comp.invProjection * vec4(ndc, 1.0);
    return view.xyz / view.w;
}

void main(){
    vec3 scene = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    vec4 refl  = texture(sampler2D(u_ssrTexture, u_ssr_smp), v_texcoord);

    int mode = int(comp.params.z + 0.5);   // debug mode (0 = off)
    float intensity = comp.params.x;

    // G-buffer is in depth space
    vec2 d = v_texcoord;
    if (comp.params.y > 0.5) d.y = 1.0 - d.y;

    float depth01 = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), d));

    // background (no geometry): nothing to reflect, the SSR mask is 0 anyway
    if (depth01 >= 1.0){
        if (mode == 0)      frag_color = vec4(scene, 1.0);
        else if (mode == 1) frag_color = vec4(refl.rgb * refl.a, 1.0);
        else                frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 alb = texture(sampler2D(u_albedoTexture, u_albedo_smp), d);
    float iblSource = alb.a; // 0 = none, 0.5 = sky, 1 = local probe

    vec4 g = texture(sampler2D(u_gbufferTexture, u_gbuffer_smp), d);
    vec3 nV = octDecode(g.rg);
    float roughness = g.b;
    float metallic = g.a;
    vec3 albedo = alb.rgb;

    vec3 P = reconstructViewPos(d, depth01);
    if (dot(nV, P) > 0.0) nV = -nV;      // orient toward camera
    vec3 vV = normalize(-P);
    float NdotV = clamp(dot(nV, vV), 0.0, 1.0);

    // GGX environment BRDF weight (FssEss): the physical reflectance of this surface,
    // including metal F0. Applied to both the IBL specular and the SSR reflection.
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec2 f_ab = envBRDFApprox(NdotV, roughness);
    vec3 Fr = max(vec3(1.0 - roughness), f0) - f0;
    vec3 k_S = f0 + Fr * pow(1.0 - NdotV, 5.0);
    vec3 FssEss = k_S * f_ab.x + f_ab.y;

    // IBL specular already present in the scene (only for surfaces that applied it).
    // Skip the environment sample entirely for non-IBL pixels.
    vec3 iblSpec = vec3(0.0);
    if (iblSource > 0.25 && iblSource < 0.75){
        vec3 rV = reflect(-vV, nV);                    // view-space reflection
        vec3 rW = normalize(mat3(comp.invView) * rV);  // -> world space for the env lookup
        float lod = roughness * ENV_GGX_LODS;
        vec3 specularLight = sRGBToLinear(textureLod(samplerCube(u_GGXEnvTexture, u_GGXEnv_smp), rotateEnv(rW, comp.envColor.w), lod)).rgb * comp.envColor.rgb;
        iblSpec = specularLight * FssEss;
    }

    // debug visualizations of the SSR G-buffer channels
    if (mode != 0){
        if (mode == 1)      frag_color = vec4(refl.rgb * refl.a, 1.0);          // reflection
        else if (mode == 2) frag_color = vec4(nV * 0.5 + 0.5, 1.0);            // view normal
        else if (mode == 3) frag_color = vec4(vec3(roughness), 1.0);          // roughness
        else if (mode == 4) frag_color = vec4(vec3(metallic), 1.0);           // metallic
        else if (mode == 5) frag_color = vec4(linearTosRGB(albedo), 1.0);     // base color
        else                frag_color = vec4(linearTosRGB(iblSpec), 1.0);    // IBL specular
        return;
    }

    // SSR reflection: un-premultiply, linearize, apply the same BRDF weight
    float mask = refl.a;
    vec3 reflColorLin = sRGBToLinear(refl.rgb / max(mask, 1e-4));
    vec3 ssrSpec = reflColorLin * FssEss;

    // Energy-conserving blend: where SSR is confident (mask) it replaces the IBL
    // specular; where it misses, the IBL reflection remains. intensity scales it.
    float w = mask * intensity;
    vec3 sceneLin = sRGBToLinear(scene);
    vec3 outLin = sceneLin + w * (ssrSpec - iblSpec);

    frag_color = vec4(linearTosRGB(max(outLin, vec3(0.0))), 1.0);
}
