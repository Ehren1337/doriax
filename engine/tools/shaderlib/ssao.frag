#version 450

// Screen-space ambient occlusion (hemisphere kernel, depth-source).
// Reconstructs the view-space position from the packed NDC depth (u_depthTexture)
// and accumulates occlusion over a randomly-rotated hemisphere kernel. The surface
// normal comes from the SSR G-buffer when available (params.w), otherwise it is
// reconstructed from the depth gradient. Output: AO factor in [0,1] (1 = unoccluded).

#ifndef SSAO_KERNEL_SIZE
#define SSAO_KERNEL_SIZE 32
#endif

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;
uniform texture2D u_noiseTexture;
uniform sampler u_noise_smp;
uniform texture2D u_gbufferTexture;   // SSR G-buffer view-space normal (oct .rg); used when params.w > 0.5
uniform sampler u_gbuffer_smp;

uniform u_fs_ssaoParams {
    mat4 projection;
    mat4 invProjection;
    vec4 kernel[SSAO_KERNEL_SIZE];
    vec4 params;     // x = radius, y = bias, z = intensity(power), w = use G-buffer normal
    vec4 noiseScale; // xy = screenSize / noiseSize (tile the noise); zw = 1/depthTextureSize
} ssao;

#include "includes/depth_util.glsl"
#include "includes/octahedral.glsl"

// No per-backend depth range here: the depth buffer packs 0.5*z/w + 0.5 taken
// before the API clip-space fixup, so [-1,1] always matches invProjection.
vec3 reconstructViewPos(vec2 uv, float depth01){
    vec3 ndc = vec3(uv * 2.0 - 1.0, depth01 * 2.0 - 1.0);
    vec4 view = ssao.invProjection * vec4(ndc, 1.0);
    return view.xyz / view.w;
}

void main(){
    float depth01 = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord));

    // background (cleared to white => depth ~1.0): no occlusion
    if (depth01 >= 1.0){
        frag_color = vec4(1.0);
        return;
    }

    vec3 P = reconstructViewPos(v_texcoord, depth01);

    vec3 n;
    if (ssao.params.w > 0.5){
        // real geometric view-space normal from the SSR G-buffer (same buffer/space
        // as the depth above), avoiding depth-gradient reconstruction artifacts
        n = octDecode(texture(sampler2D(u_gbufferTexture, u_gbuffer_smp), v_texcoord).rg);
    }else{
        // View-space normal reconstructed from depth. Naive cross(dFdx,dFdy) produces
        // garbage normals at silhouettes (the derivative straddles a depth jump) which
        // shows up as dark AO halos along edges. Instead tap the 4 neighbours and build
        // the gradient from whichever side is closer in depth, never crossing an edge.
        vec2 texel = ssao.noiseScale.zw;
        float dl = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord - vec2(texel.x, 0.0)));
        float dr = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord + vec2(texel.x, 0.0)));
        float dd = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord - vec2(0.0, texel.y)));
        float du = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord + vec2(0.0, texel.y)));
        vec3 Pl = reconstructViewPos(v_texcoord - vec2(texel.x, 0.0), dl);
        vec3 Pr = reconstructViewPos(v_texcoord + vec2(texel.x, 0.0), dr);
        vec3 Pd = reconstructViewPos(v_texcoord - vec2(0.0, texel.y), dd);
        vec3 Pu = reconstructViewPos(v_texcoord + vec2(0.0, texel.y), du);
        vec3 dpdx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
        vec3 dpdy = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
        n = normalize(cross(dpdx, dpdy));
    }
    if (dot(n, P) > 0.0) n = -n; // orient toward the camera

    // per-pixel random rotation, tiled across the screen
    vec3 randomVec = normalize(texture(sampler2D(u_noiseTexture, u_noise_smp), v_texcoord * ssao.noiseScale.xy).xyz * 2.0 - 1.0);

    vec3 tangent = normalize(randomVec - n * dot(randomVec, n));
    vec3 bitangent = cross(n, tangent);
    mat3 TBN = mat3(tangent, bitangent, n);

    float radius = ssao.params.x;
    float bias = ssao.params.y;

    float occlusion = 0.0;
    for (int i = 0; i < SSAO_KERNEL_SIZE; ++i){
        vec3 samplePos = P + (TBN * ssao.kernel[i].xyz) * radius;

        vec4 offset = ssao.projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        vec2 sampleUV = offset.xy * 0.5 + 0.5;

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
            continue;

        float sd01 = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), sampleUV));
        vec3 storedPos = reconstructViewPos(sampleUV, sd01);

        // occluded when stored geometry is closer to the camera than the sample
        // point (larger, i.e. less negative, view-space z)
        float rangeCheck = smoothstep(0.0, 1.0, radius / max(0.0001, abs(P.z - storedPos.z)));
        occlusion += (storedPos.z >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(SSAO_KERNEL_SIZE));
    occlusion = pow(clamp(occlusion, 0.0, 1.0), ssao.params.z);

    frag_color = vec4(vec3(occlusion), 1.0);
}
