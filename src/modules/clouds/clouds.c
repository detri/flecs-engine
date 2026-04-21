#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../renderer/renderer.h"
#include "flecs_engine.h"

ECS_COMPONENT_DECLARE(FlecsClouds);

typedef struct FlecsCloudsImpl {
    WGPUBuffer uniform_buffer;
    WGPUTexture weather_texture;
    WGPUTextureView weather_view;
    WGPUTexture noise_texture;
    WGPUTextureView noise_view;
    WGPUSampler repeat_sampler;
    uint8_t *weather_cpu;        /* size = WEATHER_SIZE^2 * 4 */
    double time_seconds;
    double weather_last_update;
    float weather_last_coverage;
    /* Baked cloud shadow texture (Hillaire 2016 §5.9). R8 transmittance
     * computed per-frame from the weather + noise textures using the same
     * density formula as the sky shader, projected into a world-aligned
     * footprint around the camera. PBR samples this directly by world XZ. */
    WGPUTexture shadow_texture;
    WGPUTextureView shadow_view;
    WGPUBuffer shadow_uniform_buffer;
    WGPUBindGroupLayout shadow_bake_layout;
    WGPURenderPipeline shadow_bake_pipeline;
    WGPUBindGroup shadow_bake_bind_group;
    /* World-space origin and inverse footprint of the shadow texture this
     * frame, latched here so publishShadow can re-publish them every frame. */
    float shadow_origin_x;
    float shadow_origin_z;
    float shadow_inv_footprint;
} FlecsCloudsImpl;

ECS_COMPONENT_DECLARE(FlecsCloudsImpl);

typedef struct FlecsCloudsUniform {
    mat4 inv_vp;
    float camera_pos[4];
    float sun_dir[4];        /* xyz dir, w intensity */
    float sun_color[4];
    float params0[4];        /* low_y, high_y, density_scale, time */
    float params1[4];        /* coverage_bias, weather_inv_scale, noise_inv_scale, _ */
    float params2[4];        /* wind_x, wind_z, ambient_intensity, max_dist */
    float ambient_top[4];
    float ambient_bottom[4];
} FlecsCloudsUniform;

#define FLECS_CLOUDS_WEATHER_SIZE 256u
#define FLECS_CLOUDS_NOISE_SIZE 256u
#define FLECS_CLOUDS_SHADOW_SIZE 256u
#define FLECS_CLOUDS_SHADOW_FORMAT WGPUTextureFormat_R8Unorm

typedef struct FlecsShadowBakeUniform {
    float sun_dir[4];        /* xyz, w unused; sun.y must be > 0 to bake */
    float params[4];         /* origin_x, origin_z, footprint, slab_low_y */
    float params2[4];        /* slab_high_y, weather_inv_scale, noise_inv_scale, time */
    float params3[4];        /* wind_x, wind_z, coverage_bias, density_scale */
} FlecsShadowBakeUniform;

/* Publish the baked shadow texture and its world-space footprint so PBR can
 * sample it analytically by world XZ. The shadow texture stores per-ground-
 * point cloud transmittance (Hillaire 2016 §5.9), so projection along the
 * sun direction is baked in and PBR doesn't need to know the sun direction
 * at sampling time. */
static void flecs_clouds_publishShadow(
    FlecsEngineImpl *engine,
    WGPUTextureView view,
    float origin_x,
    float origin_z,
    float inv_footprint,
    float strength)
{
    bool view_changed = engine->clouds.shadow_source_view != view;
    engine->clouds.shadow_source_view = view;
    engine->clouds.origin_x = origin_x;
    engine->clouds.origin_z = origin_z;
    engine->clouds.strength = strength;
    engine->clouds.inv_footprint = inv_footprint;
    if (view_changed) {
        engine->clouds.version++;
        engine->scene_bind_version++;
    }
}

static const char *kShadowBakeShader =
    FLECS_ENGINE_FULLSCREEN_VS_WGSL
    "struct ShadowBakeUniforms {\n"
    "  sun_dir : vec4<f32>,\n"
    "  params : vec4<f32>,\n"
    "  params2 : vec4<f32>,\n"
    "  params3 : vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u : ShadowBakeUniforms;\n"
    "@group(0) @binding(1) var weather_texture : texture_2d<f32>;\n"
    "@group(0) @binding(2) var noise_texture : texture_2d<f32>;\n"
    "@group(0) @binding(3) var repeat_sampler : sampler;\n"
    "fn remap(v : f32, lo : f32, hi : f32, nlo : f32, nhi : f32) -> f32 {\n"
    "  return nlo + (v - lo) * (nhi - nlo) / max(hi - lo, 1e-6);\n"
    "}\n"
    /* height_density at slab midpoint (h = 0.5). Same shape as the sky
     * shader's height_density evaluated at the midpoint. */
    "fn h_density_mid(cloud_type : f32) -> f32 {\n"
    "  let stratus = saturate(remap(0.5, 0.0, 0.10, 0.0, 1.0))\n"
    "              * saturate(remap(0.5, 0.20, 0.30, 1.0, 0.0));\n"
    "  let cumulus = saturate(remap(0.5, 0.05, 0.20, 0.0, 1.0))\n"
    "              * saturate(remap(0.5, 0.55, 0.85, 1.0, 0.0));\n"
    "  let cnb     = saturate(remap(0.5, 0.05, 0.30, 0.0, 1.0))\n"
    "              * saturate(remap(0.5, 0.80, 1.00, 1.0, 0.0));\n"
    "  let t = saturate(cloud_type);\n"
    "  let a = mix(stratus, cumulus, saturate(t * 2.0));\n"
    "  return mix(a, cnb, saturate(t * 2.0 - 1.0));\n"
    "}\n"
    "@fragment fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {\n"
    "  if (u.sun_dir.y < 0.05) { return vec4<f32>(1.0, 0.0, 0.0, 1.0); }\n"
    /* Map shadow texel UV -> world ground point inside the camera-aligned\n"
     * footprint. */
    "  let footprint = u.params.z;\n"
    "  let ground_xz = vec2<f32>(u.params.x, u.params.y) + in.uv * footprint;\n"
    /* Project ground point along sun ray to slab midpoint. Sample the\n"
     * cloud field at that XZ — this is the cloud column whose shadow falls\n"
     * on this ground point. */
    "  let slab_low = u.params.w;\n"
    "  let slab_high = u.params2.x;\n"
    "  let h_mid = (slab_low + slab_high) * 0.5;\n"
    "  let t = h_mid / max(u.sun_dir.y, 1e-3);\n"
    "  let cloud_xz = ground_xz + u.sun_dir.xz * t;\n"
    /* Same sampling pattern as the sky shader: weather coverage + noise\n"
     * detail, scrolled by wind. */
    "  let wind = vec2<f32>(u.params3.x, u.params3.y) * u.params2.w;\n"
    "  let w_uv = cloud_xz * u.params2.y + wind * u.params2.y * 0.05;\n"
    "  let n_uv = cloud_xz * u.params2.z + wind * u.params2.z;\n"
    "  let weather = textureSampleLevel(weather_texture, repeat_sampler, w_uv, 0.0);\n"
    "  let noise = textureSampleLevel(noise_texture, repeat_sampler, n_uv, 0.0);\n"
    "  let coverage = saturate(weather.r + u.params3.z);\n"
    "  if (coverage < 0.001) { return vec4<f32>(1.0, 0.0, 0.0, 1.0); }\n"
    "  let cloud_type = weather.b;\n"
    "  let hgrad = h_density_mid(cloud_type);\n"
    "  let base = noise.r;\n"
    "  let detail = noise.g * 0.625 + noise.b * 0.25 + noise.a * 0.125;\n"
    "  var d = base * hgrad;\n"
    "  d = saturate(remap(d, 1.0 - coverage, 1.0, 0.0, 1.0)) * coverage;\n"
    "  let erode_mask = 0.7;\n"
    "  d = saturate(d - (1.0 - detail) * 0.4 * erode_mask);\n"
    /* Approximate optical depth through the slab in the sun direction. The\n"
     * `density_scale` constant tunes how dark a fully-opaque cloud renders;\n"
     * 5 gives reasonable cumulus-like deep shadow. */
    "  let od = d * u.params3.w;\n"
    "  let trans = exp(-od);\n"
    "  return vec4<f32>(trans, 0.0, 0.0, 1.0);\n"
    "}\n";

static const char *kCloudsShader =
    FLECS_ENGINE_FULLSCREEN_VS_WGSL
    "struct CloudsUniforms {\n"
    "  inv_vp : mat4x4<f32>,\n"
    "  camera_pos : vec4<f32>,\n"
    "  sun_dir : vec4<f32>,\n"
    "  sun_color : vec4<f32>,\n"
    "  params0 : vec4<f32>,\n"
    "  params1 : vec4<f32>,\n"
    "  params2 : vec4<f32>,\n"
    "  ambient_top : vec4<f32>,\n"
    "  ambient_bottom : vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var input_texture : texture_2d<f32>;\n"
    "@group(0) @binding(1) var input_sampler : sampler;\n"
    "@group(0) @binding(2) var depth_texture : texture_depth_2d;\n"
    "@group(0) @binding(3) var<uniform> u : CloudsUniforms;\n"
    "@group(0) @binding(4) var weather_texture : texture_2d<f32>;\n"
    "@group(0) @binding(5) var noise_texture : texture_2d<f32>;\n"
    "@group(0) @binding(6) var repeat_sampler : sampler;\n"
    "const STEPS : i32 = 32;\n"
    "const LIGHT_STEPS : i32 = 4;\n"
    "fn reconstruct_world_pos(uv : vec2<f32>, depth : f32) -> vec3<f32> {\n"
    "  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);\n"
    "  let h = u.inv_vp * ndc;\n"
    "  if (abs(h.w) > 1e-6) { return h.xyz / h.w; }\n"
    "  return h.xyz;\n"
    "}\n"
    "fn remap(v : f32, lo : f32, hi : f32, nlo : f32, nhi : f32) -> f32 {\n"
    "  return nlo + (v - lo) * (nhi - nlo) / max(hi - lo, 1e-6);\n"
    "}\n"
    /* Hard-coded type-density gradients (Schneider). x = stratus, y = cumulus,
     * z = cumulonimbus. h is normalized slab height in [0,1]. */
    "fn height_density(h : f32, cloud_type : f32) -> f32 {\n"
    "  let stratus = saturate(remap(h, 0.0, 0.10, 0.0, 1.0))\n"
    "              * saturate(remap(h, 0.20, 0.30, 1.0, 0.0));\n"
    "  let cumulus = saturate(remap(h, 0.05, 0.20, 0.0, 1.0))\n"
    "              * saturate(remap(h, 0.55, 0.85, 1.0, 0.0));\n"
    "  let cnb     = saturate(remap(h, 0.05, 0.30, 0.0, 1.0))\n"
    "              * saturate(remap(h, 0.80, 1.00, 1.0, 0.0));\n"
    "  let t = saturate(cloud_type);\n"
    "  let a = mix(stratus, cumulus, saturate(t * 2.0));\n"
    "  return mix(a, cnb, saturate(t * 2.0 - 1.0));\n"
    "}\n"
    "fn sample_density(p : vec3<f32>, low_y : f32, high_y : f32) -> vec2<f32> {\n"
    /* returns (density, cloud_type) */
    "  let h = saturate((p.y - low_y) / max(high_y - low_y, 1e-3));\n"
    "  let wind = vec2<f32>(u.params2.x, u.params2.y) * u.params0.w;\n"
    "  let w_uv = p.xz * u.params1.y + wind * u.params1.y * 0.05;\n"
    "  let n_uv = p.xz * u.params1.z + wind * u.params1.z;\n"
    "  let weather = textureSampleLevel(weather_texture, repeat_sampler, w_uv, 0.0);\n"
    "  let noise = textureSampleLevel(noise_texture, repeat_sampler, n_uv, 0.0);\n"
    "  let coverage = saturate(weather.r + u.params1.x);\n"
    "  if (coverage <= 0.001) { return vec2<f32>(0.0, weather.b); }\n"
    "  let cloud_type = weather.b;\n"
    "  let hgrad = height_density(h, cloud_type);\n"
    /* Combine octaves: r = base low-freq, gba contribute as detail erosion. */
    "  let base = noise.r;\n"
    "  let detail = noise.g * 0.625 + noise.b * 0.25 + noise.a * 0.125;\n"
    "  var d = base * hgrad;\n"
    /* Schneider coverage remap. */
    "  d = saturate(remap(d, 1.0 - coverage, 1.0, 0.0, 1.0)) * coverage;\n"
    /* Erode edges with detail noise. */
    "  let erode_mask = saturate(remap(h, 0.0, 0.4, 1.0, 0.0)) * 0.5 + 0.5;\n"
    "  d = saturate(d - (1.0 - detail) * 0.4 * erode_mask);\n"
    "  return vec2<f32>(d, cloud_type);\n"
    "}\n"
    "fn hg(cos_t : f32, g : f32) -> f32 {\n"
    "  let g2 = g * g;\n"
    "  let denom = 1.0 + g2 - 2.0 * g * cos_t;\n"
    "  return (1.0 - g2) / (12.566370614 * pow(max(denom, 1e-4), 1.5));\n"
    "}\n"
    "fn dual_hg(cos_t : f32, scale : f32) -> f32 {\n"
    /* Dual-lobe Henyey-Greenstein per Hillaire 2016 §5.7. `scale` widens the
     * phase lobe (Wrenninge multi-scattering c^n term). */
    "  let g0 = 0.8 * scale;\n"
    "  let g1 = -0.2 * scale;\n"
    "  return mix(hg(cos_t, g0), hg(cos_t, g1), 0.5);\n"
    "}\n"
    "fn light_march(p_in : vec3<f32>, low_y : f32, high_y : f32) -> f32 {\n"
    /* 4 progressively larger steps toward the sun, returns optical depth. */
    "  var od : f32 = 0.0;\n"
    "  var step : f32 = 30.0;\n"
    "  var p = p_in;\n"
    "  for (var i : i32 = 0; i < LIGHT_STEPS; i = i + 1) {\n"
    "    p = p + u.sun_dir.xyz * step;\n"
    "    if (p.y > high_y || p.y < low_y) { break; }\n"
    "    let s = sample_density(p, low_y, high_y);\n"
    "    od = od + s.x * step;\n"
    "    step = step * 2.0;\n"
    "  }\n"
    "  return od;\n"
    "}\n"
    "@fragment fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {\n"
    "  let src = textureSample(input_texture, input_sampler, in.uv);\n"
    "  let dims = textureDimensions(depth_texture);\n"
    "  let dims_f = vec2<f32>(f32(dims.x), f32(dims.y));\n"
    "  let clamped_uv = clamp(in.uv, vec2<f32>(0.0), vec2<f32>(0.9999));\n"
    "  let texel = vec2<i32>(clamped_uv * dims_f);\n"
    "  let depth = textureLoad(depth_texture, texel, 0);\n"
    "  if (depth < 0.9999) { return src; }\n"
    "  let world_pos = reconstruct_world_pos(in.uv, 1.0);\n"
    "  let cam = u.camera_pos.xyz;\n"
    "  let d = normalize(world_pos - cam);\n"
    "  let low_y = u.params0.x;\n"
    "  let high_y = u.params0.y;\n"
    "  let max_dist = u.params2.w;\n"
    /* Sphere-slab intersect on a virtual planet so clouds curve below the
     * horizon. Planet center is at (cam.x, -planet_r, cam.z) so cam is at
     * altitude cam.y above the surface. */
    "  let planet_r : f32 = 6360000.0;\n"
    "  let pc = vec3<f32>(0.0, planet_r + cam.y, 0.0);\n"
    "  let bdotd = pc.y * d.y;\n"
    "  let pc2 = pc.y * pc.y;\n"
    "  let r_low = planet_r + low_y;\n"
    "  let r_high = planet_r + high_y;\n"
    "  let disc_low = bdotd * bdotd - pc2 + r_low * r_low;\n"
    "  let disc_high = bdotd * bdotd - pc2 + r_high * r_high;\n"
    "  if (disc_high < 0.0) { return src; }\n"
    "  let sqrt_high = sqrt(disc_high);\n"
    "  var t_enter : f32 = 0.0;\n"
    "  var t_exit : f32 = -bdotd + sqrt_high;\n"
    "  if (cam.y < low_y) {\n"
    /* Camera below cloud layer: enter at far root of low sphere, exit at far
     * root of high sphere. */
    "    if (disc_low < 0.0 || d.y <= 0.0) { return src; }\n"
    "    t_enter = -bdotd + sqrt(disc_low);\n"
    "  } else if (cam.y > high_y) {\n"
    /* Camera above cloud layer: enter at near root of high, exit at near root\n"
     * of low (or far root if no low intersect). */
    "    if (d.y >= 0.0) { return src; }\n"
    "    t_enter = -bdotd - sqrt_high;\n"
    "    if (disc_low >= 0.0) {\n"
    "      t_exit = -bdotd - sqrt(disc_low);\n"
    "    }\n"
    "  } else {\n"
    /* Camera inside cloud layer: start at 0, exit at first slab crossing. */
    "    if (d.y > 0.0) {\n"
    "      t_exit = -bdotd + sqrt_high;\n"
    "    } else if (disc_low >= 0.0) {\n"
    "      t_exit = -bdotd - sqrt(disc_low);\n"
    "    }\n"
    "  }\n"
    "  t_exit = min(t_exit, max_dist);\n"
    "  if (t_exit <= t_enter + 1.0) { return src; }\n"
    /* Horizon fade so the very-distant edge softens into atmosphere. */
    "  let horizon_fade = smoothstep(-0.02, 0.05, d.y);\n"
    "  let extinction = max(u.params0.z, 1e-4);\n"
    /* Cap step size so near-horizon rays don't accumulate giant slabs of\n"
     * density per sample. STEPS is the upper bound. */
    "  let max_dt : f32 = 200.0;\n"
    "  let raw_dt = (t_exit - t_enter) / f32(STEPS);\n"
    "  let dt = min(raw_dt, max_dt);\n"
    "  let actual_steps = i32(min(f32(STEPS), (t_exit - t_enter) / dt));\n"
    "  let cos_sun = dot(d, u.sun_dir.xyz);\n"
    /* Wrenninge multi-scattering octaves (Hillaire 2016 §5.8). N=3 sums
     * three increasingly-attenuated, increasingly-isotropic contributions
     * to approximate multi-scattering without re-marching. Constants per
     * Hillaire: a (in-scattering attenuation), b (extinction attenuation,
     * a <= b for energy conservation), c (phase widening). */
    "  let MS_OCT : i32 = 3;\n"
    "  let MS_A : f32 = 0.5;\n"
    "  let MS_B : f32 = 0.6;\n"
    "  let MS_C : f32 = 0.5;\n"
    "  var ms_phase : array<f32, 3>;\n"
    "  var ms_a_pow : array<f32, 3>;\n"
    "  var ms_b_pow : array<f32, 3>;\n"
    "  for (var k : i32 = 0; k < MS_OCT; k = k + 1) {\n"
    "    let kf = f32(k);\n"
    "    ms_phase[k] = dual_hg(cos_sun, pow(MS_C, kf));\n"
    "    ms_a_pow[k] = pow(MS_A, kf);\n"
    "    ms_b_pow[k] = pow(MS_B, kf);\n"
    "  }\n"
    "  var transmittance : f32 = 1.0;\n"
    "  var scattered : vec3<f32> = vec3<f32>(0.0);\n"
    /* Stochastic offset per pixel to break banding. */
    "  let jitter = fract(sin(dot(in.uv * dims_f, vec2<f32>(12.9898, 78.233))) * 43758.5453);\n"
    "  var t = t_enter + dt * jitter;\n"
    "  for (var i : i32 = 0; i < actual_steps; i = i + 1) {\n"
    "    if (transmittance < 0.01) { break; }\n"
    "    let p = cam + d * t;\n"
    "    let s = sample_density(p, low_y, high_y);\n"
    "    let dens = s.x;\n"
    "    if (dens > 0.001) {\n"
    "      let sigma_t = dens * extinction;\n"
    "      let light_od = light_march(p, low_y, high_y) * extinction;\n"
    "      let sun_t = exp(-light_od);\n"
    /* Powder term (Schneider 2015 §57): in-scattering depth boost; clamp so\n"
     * thin clouds aren't darkened to zero. */
    "      let powder = mix(1.0, 1.0 - exp(-light_od * 2.0), 0.5);\n"
    "      let h_norm = saturate((p.y - low_y) / max(high_y - low_y, 1e-3));\n"
    "      let ambient = mix(u.ambient_bottom.rgb, u.ambient_top.rgb, h_norm)\n"
    "                  * u.params2.z;\n"
    /* Hillaire 2016 Eq. 17: integrand is sigma_s * Lscat; for clouds albedo\n"
     * ~= 1 so sigma_s ~= sigma_t. integ then collapses to:\n"
     *   Lscat * (1 - exp(-sigma_t*dt))                                       */
    /* Sum N octaves of scattering. Each octave attenuates the in-scatter
     * (a^k) and shadowed sun term (b^k via Beer^(b^k)) and broadens the
     * phase function. Octave 0 == single-scattering; higher octaves add
     * the diffuse multi-scatter halo that makes thick clouds look puffy
     * rather than smoky. */
    "      var Lscat = vec3<f32>(0.0);\n"
    "      for (var k : i32 = 0; k < MS_OCT; k = k + 1) {\n"
    "        let sun_t_k = pow(sun_t, ms_b_pow[k]);\n"
    "        let sun_term = u.sun_color.rgb * sun_t_k * ms_phase[k] * powder;\n"
    "        Lscat = Lscat + (sun_term + ambient * ms_a_pow[k]) * ms_a_pow[k];\n"
    "      }\n"
    "      let trans_step = exp(-sigma_t * dt);\n"
    "      let integ = Lscat * (1.0 - trans_step);\n"
    "      scattered = scattered + transmittance * integ;\n"
    "      transmittance = transmittance * trans_step;\n"
    "    }\n"
    "    t = t + dt;\n"
    "  }\n"
    "  let alpha = (1.0 - transmittance) * horizon_fade;\n"
    "  let composed = mix(src.rgb, src.rgb * transmittance + scattered, horizon_fade);\n"
    "  return vec4<f32>(composed, src.a + alpha * (1.0 - src.a));\n"
    "}\n";

static ecs_entity_t flecsEngine_clouds_shader(
    ecs_world_t *world)
{
    return flecsEngine_shader_ensure(world, "CloudsShader",
        &(FlecsShader){
            .source = kCloudsShader,
            .vertex_entry = "vs_main",
            .fragment_entry = "fs_main"
        });
}

static void flecsEngine_clouds_releaseResources(
    FlecsCloudsImpl *impl)
{
    FLECS_WGPU_RELEASE(impl->uniform_buffer, wgpuBufferRelease);
    FLECS_WGPU_RELEASE(impl->weather_view, wgpuTextureViewRelease);
    FLECS_WGPU_RELEASE(impl->weather_texture, wgpuTextureRelease);
    FLECS_WGPU_RELEASE(impl->noise_view, wgpuTextureViewRelease);
    FLECS_WGPU_RELEASE(impl->noise_texture, wgpuTextureRelease);
    FLECS_WGPU_RELEASE(impl->repeat_sampler, wgpuSamplerRelease);
    FLECS_WGPU_RELEASE(impl->shadow_view, wgpuTextureViewRelease);
    FLECS_WGPU_RELEASE(impl->shadow_texture, wgpuTextureRelease);
    FLECS_WGPU_RELEASE(impl->shadow_uniform_buffer, wgpuBufferRelease);
    FLECS_WGPU_RELEASE(impl->shadow_bake_bind_group, wgpuBindGroupRelease);
    FLECS_WGPU_RELEASE(impl->shadow_bake_pipeline, wgpuRenderPipelineRelease);
    FLECS_WGPU_RELEASE(impl->shadow_bake_layout, wgpuBindGroupLayoutRelease);
    if (impl->weather_cpu) {
        ecs_os_free(impl->weather_cpu);
        impl->weather_cpu = NULL;
    }
}

ECS_DTOR(FlecsCloudsImpl, ptr, {
    flecsEngine_clouds_releaseResources(ptr);
})

ECS_MOVE(FlecsCloudsImpl, dst, src, {
    flecsEngine_clouds_releaseResources(dst);
    *dst = *src;
    ecs_os_zeromem(src);
})

static uint32_t flecs_clouds_hash(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float flecs_clouds_value_noise(float x, float y, int32_t period, uint32_t seed)
{
    int32_t xi = (int32_t)floorf(x);
    int32_t yi = (int32_t)floorf(y);
    float fx = x - (float)xi;
    float fy = y - (float)yi;
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sy = fy * fy * (3.0f - 2.0f * fy);
    /* Wrap for tileable output. */
    int32_t x0 = ((xi % period) + period) % period;
    int32_t y0 = ((yi % period) + period) % period;
    int32_t x1 = (x0 + 1) % period;
    int32_t y1 = (y0 + 1) % period;
    float n00 = (float)(flecs_clouds_hash(x0, y0, seed) & 0xFFFF) / 65535.0f;
    float n10 = (float)(flecs_clouds_hash(x1, y0, seed) & 0xFFFF) / 65535.0f;
    float n01 = (float)(flecs_clouds_hash(x0, y1, seed) & 0xFFFF) / 65535.0f;
    float n11 = (float)(flecs_clouds_hash(x1, y1, seed) & 0xFFFF) / 65535.0f;
    float a = n00 * (1.0f - sx) + n10 * sx;
    float b = n01 * (1.0f - sx) + n11 * sx;
    return a * (1.0f - sy) + b * sy;
}

static float flecs_clouds_fbm(float x, float y, int32_t base_period, int32_t octaves, uint32_t seed)
{
    float sum = 0.0f;
    float amp = 0.5f;
    float total = 0.0f;
    int32_t period = base_period;
    for (int32_t i = 0; i < octaves; i++) {
        sum += amp * flecs_clouds_value_noise(x * (float)period / (float)base_period,
            y * (float)period / (float)base_period, period, seed + (uint32_t)i);
        total += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / total;
}

static void flecs_clouds_bakeWeather(
    uint8_t *data, float coverage_bias, float time_offset)
{
    /* RGBA8: R = coverage, G = precipitation, B = cloud_type, A = unused.
     * Initial bake; subsequent frames evolve `data` via flecs_clouds_evolveWeather.
     * Used at startup to seed the CA. */
    const uint32_t s = FLECS_CLOUDS_WEATHER_SIZE;
    float scroll = time_offset * 0.02f;
    for (uint32_t y = 0; y < s; y++) {
        for (uint32_t x = 0; x < s; x++) {
            float u = (float)x / (float)s * 4.0f + scroll;
            float v = (float)y / (float)s * 4.0f + scroll * 0.7f;
            float cov_raw = flecs_clouds_fbm(u, v, 4, 4, 11u);
            float cov = (cov_raw - 0.5f + (coverage_bias - 0.5f)) * 2.5f + 0.5f;
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            float type_raw = flecs_clouds_fbm(
                u * 0.7f + 3.0f, v * 0.7f + 3.0f, 4, 3, 23u);
            float type = type_raw;
            if (type < 0.0f) type = 0.0f;
            if (type > 1.0f) type = 1.0f;
            uint8_t *px = &data[(y * s + x) * 4];
            px[0] = (uint8_t)(cov * 255.0f);
            px[1] = 0u;
            px[2] = (uint8_t)(type * 255.0f);
            px[3] = 255u;
        }
    }
}

/* Cellular automaton step on the weather coverage channel. Models three
 * simple processes:
 *   - Wind advection: shift the coverage field in the wind direction.
 *   - Diffusion: 3x3 box smoothing models how clumps soften and merge.
 *   - Forcing: blend toward a slowly-scrolled FBM target so the field doesn't
 *     decay to a uniform value (the FBM acts as a synoptic-scale forcing).
 * Cloud-type channel is updated by lighter forcing alone; it changes more
 * slowly than coverage, matching real weather where storm-vs-fair-weather
 * regimes evolve on slower timescales. */
static void flecs_clouds_evolveWeather(
    uint8_t *data,
    float coverage_bias,
    float time_offset,
    float dt_seconds,
    float wind_x,
    float wind_z)
{
    const int32_t s = (int32_t)FLECS_CLOUDS_WEATHER_SIZE;
    /* Snapshot of previous step so neighbor reads are coherent. Re-allocated
     * each step (small at 256k) — could be cached on the impl if hot. */
    uint8_t *prev = ecs_os_malloc(s * s * 4);
    memcpy(prev, data, (size_t)s * s * 4);

    /* Convert wind (world units / s) into texel offsets. The texture covers
     * weather_scale_km in world space; one texel is ~weather_scale_km*1000/s
     * meters wide. We approximate weather_scale = 40 km here for the CA
     * forcing strength; the CA isn't physically calibrated. */
    float texels_per_meter = (float)s / 40000.0f;
    float dx = -wind_x * dt_seconds * texels_per_meter;  /* downwind shift */
    float dz = -wind_z * dt_seconds * texels_per_meter;
    int32_t shift_x = (int32_t)dx;
    int32_t shift_z = (int32_t)dz;

    /* FBM forcing target — same shape as initial bake, scrolls slowly so the
     * CA has something to relax toward. */
    float scroll = time_offset * 0.02f;

    for (int32_t y = 0; y < s; y++) {
        for (int32_t x = 0; x < s; x++) {
            /* Sample neighbors of the advected source position. */
            int32_t sx = ((x + shift_x) % s + s) % s;
            int32_t sy = ((y + shift_z) % s + s) % s;

            /* 3x3 box-blur on R channel from the snapshot. */
            float sum_r = 0.0f;
            float sum_b = 0.0f;
            for (int32_t dy = -1; dy <= 1; dy++) {
                for (int32_t ddx = -1; ddx <= 1; ddx++) {
                    int32_t nx = ((sx + ddx) % s + s) % s;
                    int32_t ny = ((sy + dy) % s + s) % s;
                    sum_r += (float)prev[(ny * s + nx) * 4 + 0];
                    sum_b += (float)prev[(ny * s + nx) * 4 + 2];
                }
            }
            float diffused_r = sum_r / 9.0f / 255.0f;
            float diffused_b = sum_b / 9.0f / 255.0f;

            /* Forcing target: same FBM as the initial bake. */
            float u = (float)x / (float)s * 4.0f + scroll;
            float v = (float)y / (float)s * 4.0f + scroll * 0.7f;
            float fbm_raw = flecs_clouds_fbm(u, v, 4, 4, 11u);
            float target_r =
                (fbm_raw - 0.5f + (coverage_bias - 0.5f)) * 2.5f + 0.5f;
            if (target_r < 0.0f) target_r = 0.0f;
            if (target_r > 1.0f) target_r = 1.0f;
            float type_raw = flecs_clouds_fbm(
                u * 0.7f + 3.0f, v * 0.7f + 3.0f, 4, 3, 23u);
            if (type_raw < 0.0f) type_raw = 0.0f;
            if (type_raw > 1.0f) type_raw = 1.0f;

            /* Blend toward target — small amount per step gives a long
             * relaxation time so transients are visible. Coverage relaxes
             * faster than cloud type. */
            float lerp_r = 1.0f - expf(-dt_seconds * 0.6f);
            float lerp_b = 1.0f - expf(-dt_seconds * 0.15f);
            float new_r = diffused_r + (target_r - diffused_r) * lerp_r;
            float new_b = diffused_b + (type_raw - diffused_b) * lerp_b;

            if (new_r < 0.0f) new_r = 0.0f;
            if (new_r > 1.0f) new_r = 1.0f;
            if (new_b < 0.0f) new_b = 0.0f;
            if (new_b > 1.0f) new_b = 1.0f;

            uint8_t *px = &data[(y * s + x) * 4];
            px[0] = (uint8_t)(new_r * 255.0f);
            px[1] = 0u;
            px[2] = (uint8_t)(new_b * 255.0f);
            px[3] = 255u;
        }
    }

    ecs_os_free(prev);
}

static void flecs_clouds_bakeNoise(uint8_t *data)
{
    /* RGBA8 packed octaves: R = base low-freq, G/B/A = increasing detail. */
    const uint32_t s = FLECS_CLOUDS_NOISE_SIZE;
    for (uint32_t y = 0; y < s; y++) {
        for (uint32_t x = 0; x < s; x++) {
            float u = (float)x / (float)s;
            float v = (float)y / (float)s;
            float r = flecs_clouds_fbm(u * 4.0f, v * 4.0f, 4, 5, 41u);
            float g = flecs_clouds_fbm(u * 8.0f, v * 8.0f, 8, 4, 53u);
            float b = flecs_clouds_fbm(u * 16.0f, v * 16.0f, 16, 3, 67u);
            float a = flecs_clouds_fbm(u * 32.0f, v * 32.0f, 32, 2, 79u);
            uint8_t *px = &data[(y * s + x) * 4];
            px[0] = (uint8_t)(r * 255.0f);
            px[1] = (uint8_t)(g * 255.0f);
            px[2] = (uint8_t)(b * 255.0f);
            px[3] = (uint8_t)(a * 255.0f);
        }
    }
}

static bool flecs_clouds_uploadTexture(
    const FlecsEngineImpl *engine,
    uint32_t size,
    const uint8_t *data,
    WGPUTexture *out_texture,
    WGPUTextureView *out_view)
{
    WGPUTextureDescriptor desc = {
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = WGPUTextureDimension_2D,
        .size = { size, size, 1 },
        .format = WGPUTextureFormat_RGBA8Unorm,
        .mipLevelCount = 1,
        .sampleCount = 1
    };
    WGPUTexture tex = wgpuDeviceCreateTexture(engine->device, &desc);
    if (!tex) return false;
    WGPUTexelCopyTextureInfo dst = {
        .texture = tex,
        .mipLevel = 0,
        .origin = { 0, 0, 0 },
        .aspect = WGPUTextureAspect_All
    };
    WGPUTexelCopyBufferLayout layout = {
        .offset = 0,
        .bytesPerRow = size * 4u,
        .rowsPerImage = size
    };
    WGPUExtent3D extent = { size, size, 1 };
    wgpuQueueWriteTexture(
        engine->queue, &dst, data, (size_t)size * size * 4u, &layout, &extent);
    WGPUTextureView view = wgpuTextureCreateView(tex, &(WGPUTextureViewDescriptor){
        .format = WGPUTextureFormat_RGBA8Unorm,
        .dimension = WGPUTextureViewDimension_2D,
        .mipLevelCount = 1,
        .arrayLayerCount = 1
    });
    if (!view) {
        wgpuTextureRelease(tex);
        return false;
    }
    *out_texture = tex;
    *out_view = view;
    return true;
}

static WGPUSampler flecs_clouds_createRepeatSampler(WGPUDevice device)
{
    return wgpuDeviceCreateSampler(device, &(WGPUSamplerDescriptor){
        .addressModeU = WGPUAddressMode_Repeat,
        .addressModeV = WGPUAddressMode_Repeat,
        .addressModeW = WGPUAddressMode_Repeat,
        .magFilter = WGPUFilterMode_Linear,
        .minFilter = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Linear,
        .maxAnisotropy = 1
    });
}

static bool flecsEngine_clouds_setup(
    const ecs_world_t *world,
    const FlecsEngineImpl *engine,
    ecs_entity_t effect_entity,
    const FlecsRenderEffect *effect,
    FlecsRenderEffectImpl *effect_impl,
    WGPUBindGroupLayoutEntry *layout_entries,
    uint32_t *entry_count)
{
    (void)effect;
    (void)effect_impl;

    FlecsCloudsImpl impl = {0};
    impl.uniform_buffer = flecsEngine_createUniformBuffer(
        engine->device, sizeof(FlecsCloudsUniform));
    if (!impl.uniform_buffer) return false;

    impl.weather_cpu = ecs_os_malloc(
        FLECS_CLOUDS_WEATHER_SIZE * FLECS_CLOUDS_WEATHER_SIZE * 4u);
    flecs_clouds_bakeWeather(impl.weather_cpu, 0.5f, 0.0f);
    bool ok = flecs_clouds_uploadTexture(
        engine, FLECS_CLOUDS_WEATHER_SIZE, impl.weather_cpu,
        &impl.weather_texture, &impl.weather_view);
    if (!ok) {
        ecs_os_free(impl.weather_cpu);
        wgpuBufferRelease(impl.uniform_buffer);
        return false;
    }
    impl.weather_last_coverage = 0.5f;
    impl.weather_last_update = 0.0;

    uint8_t *noise_data = ecs_os_malloc(
        FLECS_CLOUDS_NOISE_SIZE * FLECS_CLOUDS_NOISE_SIZE * 4u);
    flecs_clouds_bakeNoise(noise_data);
    ok = flecs_clouds_uploadTexture(
        engine, FLECS_CLOUDS_NOISE_SIZE, noise_data,
        &impl.noise_texture, &impl.noise_view);
    ecs_os_free(noise_data);
    if (!ok) {
        flecsEngine_clouds_releaseResources(&impl);
        return false;
    }

    impl.repeat_sampler = flecs_clouds_createRepeatSampler(engine->device);
    if (!impl.repeat_sampler) {
        flecsEngine_clouds_releaseResources(&impl);
        return false;
    }

    layout_entries[2] = (WGPUBindGroupLayoutEntry){
        .binding = 2,
        .visibility = WGPUShaderStage_Fragment,
        .texture = {
            .sampleType = WGPUTextureSampleType_Depth,
            .viewDimension = WGPUTextureViewDimension_2D,
            .multisampled = false
        }
    };
    layout_entries[3] = (WGPUBindGroupLayoutEntry){
        .binding = 3,
        .visibility = WGPUShaderStage_Fragment,
        .buffer = {
            .type = WGPUBufferBindingType_Uniform,
            .minBindingSize = sizeof(FlecsCloudsUniform)
        }
    };
    layout_entries[4] = (WGPUBindGroupLayoutEntry){
        .binding = 4,
        .visibility = WGPUShaderStage_Fragment,
        .texture = {
            .sampleType = WGPUTextureSampleType_Float,
            .viewDimension = WGPUTextureViewDimension_2D,
            .multisampled = false
        }
    };
    layout_entries[5] = (WGPUBindGroupLayoutEntry){
        .binding = 5,
        .visibility = WGPUShaderStage_Fragment,
        .texture = {
            .sampleType = WGPUTextureSampleType_Float,
            .viewDimension = WGPUTextureViewDimension_2D,
            .multisampled = false
        }
    };
    layout_entries[6] = (WGPUBindGroupLayoutEntry){
        .binding = 6,
        .visibility = WGPUShaderStage_Fragment,
        .sampler = { .type = WGPUSamplerBindingType_Filtering }
    };

    /* --- Shadow bake resources (M4) --- */
    {
        WGPUTextureDescriptor sd = {
            .usage = WGPUTextureUsage_RenderAttachment
                   | WGPUTextureUsage_TextureBinding,
            .dimension = WGPUTextureDimension_2D,
            .size = { FLECS_CLOUDS_SHADOW_SIZE, FLECS_CLOUDS_SHADOW_SIZE, 1 },
            .format = FLECS_CLOUDS_SHADOW_FORMAT,
            .mipLevelCount = 1,
            .sampleCount = 1
        };
        impl.shadow_texture = wgpuDeviceCreateTexture(engine->device, &sd);
        if (!impl.shadow_texture) {
            flecsEngine_clouds_releaseResources(&impl);
            return false;
        }
        impl.shadow_view = wgpuTextureCreateView(impl.shadow_texture,
            &(WGPUTextureViewDescriptor){
                .format = FLECS_CLOUDS_SHADOW_FORMAT,
                .dimension = WGPUTextureViewDimension_2D,
                .mipLevelCount = 1,
                .arrayLayerCount = 1
            });
        if (!impl.shadow_view) {
            flecsEngine_clouds_releaseResources(&impl);
            return false;
        }

        impl.shadow_uniform_buffer = flecsEngine_createUniformBuffer(
            engine->device, sizeof(FlecsShadowBakeUniform));
        if (!impl.shadow_uniform_buffer) {
            flecsEngine_clouds_releaseResources(&impl);
            return false;
        }

        WGPUBindGroupLayoutEntry bake_entries[4] = {
            {
                .binding = 0,
                .visibility = WGPUShaderStage_Fragment,
                .buffer = {
                    .type = WGPUBufferBindingType_Uniform,
                    .minBindingSize = sizeof(FlecsShadowBakeUniform)
                }
            },
            {
                .binding = 1,
                .visibility = WGPUShaderStage_Fragment,
                .texture = {
                    .sampleType = WGPUTextureSampleType_Float,
                    .viewDimension = WGPUTextureViewDimension_2D
                }
            },
            {
                .binding = 2,
                .visibility = WGPUShaderStage_Fragment,
                .texture = {
                    .sampleType = WGPUTextureSampleType_Float,
                    .viewDimension = WGPUTextureViewDimension_2D
                }
            },
            {
                .binding = 3,
                .visibility = WGPUShaderStage_Fragment,
                .sampler = { .type = WGPUSamplerBindingType_Filtering }
            }
        };
        impl.shadow_bake_layout = wgpuDeviceCreateBindGroupLayout(
            engine->device, &(WGPUBindGroupLayoutDescriptor){
                .entryCount = 4, .entries = bake_entries
            });
        if (!impl.shadow_bake_layout) {
            flecsEngine_clouds_releaseResources(&impl);
            return false;
        }

        WGPUShaderModule bake_mod = flecsEngine_createShaderModule(
            engine->device, kShadowBakeShader);
        if (!bake_mod) {
            flecsEngine_clouds_releaseResources(&impl);
            return false;
        }
        WGPUColorTargetState bake_target = {
            .format = FLECS_CLOUDS_SHADOW_FORMAT,
            .writeMask = WGPUColorWriteMask_All
        };
        impl.shadow_bake_pipeline = flecsEngine_createFullscreenPipeline(
            engine, bake_mod, impl.shadow_bake_layout,
            "vs_main", "fs_main", &bake_target, NULL);
        wgpuShaderModuleRelease(bake_mod);
        if (!impl.shadow_bake_pipeline) {
            flecsEngine_clouds_releaseResources(&impl);
            return false;
        }

        WGPUBindGroupEntry bake_bind[4] = {
            { .binding = 0, .buffer = impl.shadow_uniform_buffer,
              .size = sizeof(FlecsShadowBakeUniform) },
            { .binding = 1, .textureView = impl.weather_view },
            { .binding = 2, .textureView = impl.noise_view },
            { .binding = 3, .sampler = impl.repeat_sampler }
        };
        impl.shadow_bake_bind_group = wgpuDeviceCreateBindGroup(
            engine->device, &(WGPUBindGroupDescriptor){
                .layout = impl.shadow_bake_layout,
                .entryCount = 4, .entries = bake_bind
            });
        if (!impl.shadow_bake_bind_group) {
            flecsEngine_clouds_releaseResources(&impl);
            return false;
        }

        /* Initial bake at setup so frame 1 PBR has valid shadow data. PBR
         * runs in the geometry phase BEFORE the cloud effect's render
         * callback, so without this initial bake, frame 1 sees a blank
         * shadow texture. We use sensible defaults: a slab footprint
         * centered at world origin with the user's coverage and a default
         * sun direction (straight up). Subsequent frames re-bake with the
         * real sun direction in render_callback. */
        const FlecsClouds *initial = ecs_get(world, effect_entity, FlecsClouds);
        FlecsClouds defaults_init = flecsEngine_cloudsSettingsDefault();
        if (!initial) initial = &defaults_init;
        float footprint_init = initial->shadow_scale_km > 0.001f
            ? initial->shadow_scale_km * 1000.0f : 4000.0f;
        float low_y_init = initial->low_altitude_km * 1000.0f;
        float high_y_init = initial->high_altitude_km * 1000.0f;
        FlecsShadowBakeUniform sb = {0};
        sb.sun_dir[0] = 0.0f;
        sb.sun_dir[1] = 1.0f;
        sb.sun_dir[2] = 0.0f;
        sb.params[0] = -footprint_init * 0.5f;
        sb.params[1] = -footprint_init * 0.5f;
        sb.params[2] = footprint_init;
        sb.params[3] = low_y_init;
        sb.params2[0] = high_y_init;
        sb.params2[1] = 1.0f /
            (initial->weather_scale_km > 0.001f
                ? initial->weather_scale_km * 1000.0f : 20000.0f);
        sb.params2[2] = 1.0f /
            (initial->noise_scale_km > 0.001f
                ? initial->noise_scale_km * 1000.0f : 4000.0f);
        sb.params2[3] = 0.0f;
        sb.params3[0] = initial->wind_x;
        sb.params3[1] = initial->wind_z;
        sb.params3[2] = 0.0f;  /* coverage is baked into weather.r */
        /* Optical depth scale: clouds->density is the per-sample extinction
         * the sky shader uses. Multiplied by an empirical slab constant so a
         * typical density (0.05) gives visually-correct cumulus shadows. */
        sb.params3[3] = initial->density * 80.0f;
        wgpuQueueWriteBuffer(engine->queue, impl.shadow_uniform_buffer, 0,
            &sb, sizeof(sb));

        WGPUCommandEncoder init_enc = wgpuDeviceCreateCommandEncoder(
            engine->device, NULL);
        if (init_enc) {
            WGPURenderPassColorAttachment att = {
                .view = impl.shadow_view,
                WGPU_DEPTH_SLICE
                .loadOp = WGPULoadOp_Clear,
                .storeOp = WGPUStoreOp_Store,
                .clearValue = (WGPUColor){ 1.0, 1.0, 1.0, 1.0 }
            };
            WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(
                init_enc, &(WGPURenderPassDescriptor){
                    .colorAttachmentCount = 1,
                    .colorAttachments = &att
                });
            wgpuRenderPassEncoderSetPipeline(p, impl.shadow_bake_pipeline);
            wgpuRenderPassEncoderSetBindGroup(p, 0,
                impl.shadow_bake_bind_group, 0, NULL);
            wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(p);
            wgpuRenderPassEncoderRelease(p);
            WGPUCommandBuffer cb = wgpuCommandEncoderFinish(init_enc, NULL);
            wgpuQueueSubmit(engine->queue, 1, &cb);
            wgpuCommandBufferRelease(cb);
            wgpuCommandEncoderRelease(init_enc);
        }
    }

    /* Publish initial shadow registration: zero strength so the first frame
     * doesn't render with stale shadow data. The bind callback re-publishes
     * with the up-to-date origin/footprint after the first bake. */
    {
        const FlecsClouds *initial = ecs_get(world, effect_entity, FlecsClouds);
        FlecsClouds defaults = flecsEngine_cloudsSettingsDefault();
        if (!initial) initial = &defaults;
        float footprint_m = initial->shadow_scale_km > 0.001f
            ? initial->shadow_scale_km * 1000.0f : 4000.0f;
        flecs_clouds_publishShadow(
            (FlecsEngineImpl*)engine, impl.shadow_view,
            -footprint_m * 0.5f, -footprint_m * 0.5f,
            1.0f / footprint_m, initial->shadow_strength);
    }

    ecs_set_ptr((ecs_world_t*)world, effect_entity, FlecsCloudsImpl, &impl);
    *entry_count = 7;
    return true;
}

static void flecs_clouds_fillUniform(
    const ecs_world_t *world,
    ecs_entity_t effect_entity,
    const FlecsClouds *clouds,
    FlecsCloudsImpl *impl,
    float delta_seconds,
    FlecsCloudsUniform *uniform)
{
    glm_mat4_identity(uniform->inv_vp);
    impl->time_seconds += delta_seconds;

    ecs_entity_t view_entity = ecs_get_target(world, effect_entity, EcsChildOf, 0);
    const FlecsRenderView *view = view_entity
        ? ecs_get(world, view_entity, FlecsRenderView) : NULL;
    const FlecsCameraImpl *camera = (view && view->camera)
        ? ecs_get(world, view->camera, FlecsCameraImpl) : NULL;
    if (camera) {
        mat4 mvp;
        glm_mat4_copy((vec4*)camera->mvp, mvp);
        glm_mat4_inv(mvp, uniform->inv_vp);
    }

    const FlecsWorldTransform3 *cam_xf = (view && view->camera)
        ? ecs_get(world, view->camera, FlecsWorldTransform3) : NULL;
    if (cam_xf) {
        uniform->camera_pos[0] = cam_xf->m[3][0];
        uniform->camera_pos[1] = cam_xf->m[3][1];
        uniform->camera_pos[2] = cam_xf->m[3][2];
        uniform->camera_pos[3] = 1.0f;
    }

    /* Sun direction & color from atmosphere's sun light. The shader expects
     * sun_dir to point FROM the surface TOWARD the sun (light source). */
    uniform->sun_dir[0] = 0.0f;
    uniform->sun_dir[1] = 1.0f;
    uniform->sun_dir[2] = 0.0f;
    uniform->sun_color[0] = 5.0f;
    uniform->sun_color[1] = 5.0f;
    uniform->sun_color[2] = 5.0f;
    if (clouds->atmosphere) {
        const FlecsAtmosphere *atm = ecs_get(world, clouds->atmosphere, FlecsAtmosphere);
        if (atm && atm->sun) {
            const FlecsRotation3 *rot = ecs_get(world, atm->sun, FlecsRotation3);
            if (rot) {
                vec3 light_ray;
                if (flecsEngine_lightDirFromRotation(rot, light_ray)) {
                    /* light_ray points from sun toward scene; flip for sun_dir. */
                    uniform->sun_dir[0] = -light_ray[0];
                    uniform->sun_dir[1] = -light_ray[1];
                    uniform->sun_dir[2] = -light_ray[2];
                }
            }
            /* Use the atmosphere-attenuated directional light intensity (the
             * same value PBR sees for surface lighting). FlecsRgba is the
             * surface-clamped color in [0,1]; FlecsDirectionalLight.intensity
             * is the scalar brightness (TOA * transmittance * disk-fade). */
            const FlecsRgba *sun_rgb = ecs_get(world, atm->sun, FlecsRgba);
            const FlecsDirectionalLight *dl = ecs_get(
                world, atm->sun, FlecsDirectionalLight);
            if (sun_rgb && dl) {
                float scale = dl->intensity;
                uniform->sun_color[0] =
                    flecsEngine_colorChannelToFloat(sun_rgb->r) * scale;
                uniform->sun_color[1] =
                    flecsEngine_colorChannelToFloat(sun_rgb->g) * scale;
                uniform->sun_color[2] =
                    flecsEngine_colorChannelToFloat(sun_rgb->b) * scale;
            }
        }
    }

    float low_y = clouds->low_altitude_km * 1000.0f;
    float high_y = clouds->high_altitude_km * 1000.0f;
    if (clouds->atmosphere) {
        const FlecsAtmosphere *atm = ecs_get(world, clouds->atmosphere, FlecsAtmosphere);
        if (atm && atm->world_units_per_km > 1e-3f) {
            low_y = atm->sea_level_y +
                clouds->low_altitude_km * atm->world_units_per_km;
            high_y = atm->sea_level_y +
                clouds->high_altitude_km * atm->world_units_per_km;
        }
    }
    uniform->params0[0] = low_y;
    uniform->params0[1] = high_y;
    uniform->params0[2] = clouds->density;
    uniform->params0[3] = (float)impl->time_seconds;

    /* Coverage already baked into weather.r by the CPU update path; shader
     * bias stays at 0 so the slider doesn't double-apply. */
    uniform->params1[0] = 0.0f;
    uniform->params1[1] = 1.0f /
        (clouds->weather_scale_km > 0.001f ? clouds->weather_scale_km * 1000.0f : 20000.0f);
    uniform->params1[2] = 1.0f /
        (clouds->noise_scale_km > 0.001f ? clouds->noise_scale_km * 1000.0f : 4000.0f);
    uniform->params1[3] = 0.0f;

    uniform->params2[0] = clouds->wind_x;
    uniform->params2[1] = clouds->wind_z;
    uniform->params2[2] = 1.0f;
    uniform->params2[3] = 200000.0f; /* max march distance in world units */

    uniform->ambient_top[0] =
        flecsEngine_colorChannelToFloat(clouds->ambient_top.r);
    uniform->ambient_top[1] =
        flecsEngine_colorChannelToFloat(clouds->ambient_top.g);
    uniform->ambient_top[2] =
        flecsEngine_colorChannelToFloat(clouds->ambient_top.b);
    uniform->ambient_top[3] = 1.0f;
    uniform->ambient_bottom[0] =
        flecsEngine_colorChannelToFloat(clouds->ambient_bottom.r);
    uniform->ambient_bottom[1] =
        flecsEngine_colorChannelToFloat(clouds->ambient_bottom.g);
    uniform->ambient_bottom[2] =
        flecsEngine_colorChannelToFloat(clouds->ambient_bottom.b);
    uniform->ambient_bottom[3] = 1.0f;
}

/* Per-frame state update: writes both uniform buffers (composite + bake),
 * publishes shadow registration, and runs the weather CA. Called from
 * render_callback BEFORE the bake pass so the bake reads fresh uniforms.
 * bind_callback then only fills bind-group entries. */
static bool flecs_clouds_updateState(
    const ecs_world_t *world,
    const FlecsEngineImpl *engine,
    ecs_entity_t effect_entity,
    FlecsCloudsImpl *impl,
    const FlecsClouds *clouds)
{
    float delta = (float)ecs_get_world_info(world)->delta_time;
    if (delta < 0.0f) delta = 0.0f;
    if (delta > 0.1f) delta = 0.1f;
    /* time_scale scales all progression uniformly (sky scroll, weather CA,
     * wind advection). 0 freezes clouds completely, 1 is normal, >1 is
     * faster. */
    delta = delta * clouds->time_scale;

    FlecsCloudsUniform uniform = {0};
    flecs_clouds_fillUniform(world, effect_entity, clouds, impl, delta, &uniform);
    wgpuQueueWriteBuffer(engine->queue, impl->uniform_buffer, 0,
        &uniform, sizeof(uniform));

    /* M4: bake the cloud shadow texture this frame. We center the shadow
     * footprint on the camera (snapped to texel-multiple to avoid sub-texel
     * crawling when the camera moves), fill the bake uniform, and publish
     * the texture + projection params for PBR. The actual bake pass runs in
     * the render_callback before the cloud composite. */
    {
        float footprint = clouds->shadow_scale_km > 0.001f
            ? clouds->shadow_scale_km * 1000.0f : 4000.0f;
        float texel = footprint / (float)FLECS_CLOUDS_SHADOW_SIZE;
        float cx = uniform.camera_pos[0];
        float cz = uniform.camera_pos[2];
        /* Snap camera position to texel grid so shadow content is stable as
         * the camera moves; shadow texels still represent the same world XZ
         * regions until the camera crosses a full texel. */
        float snap_x = floorf(cx / texel) * texel;
        float snap_z = floorf(cz / texel) * texel;
        float origin_x = snap_x - footprint * 0.5f;
        float origin_z = snap_z - footprint * 0.5f;
        impl->shadow_origin_x = origin_x;
        impl->shadow_origin_z = origin_z;
        impl->shadow_inv_footprint = 1.0f / footprint;

        FlecsShadowBakeUniform sb = {0};
        sb.sun_dir[0] = uniform.sun_dir[0];
        sb.sun_dir[1] = uniform.sun_dir[1];
        sb.sun_dir[2] = uniform.sun_dir[2];
        sb.params[0] = origin_x;
        sb.params[1] = origin_z;
        sb.params[2] = footprint;
        sb.params[3] = uniform.params0[0];          /* slab_low_y */
        sb.params2[0] = uniform.params0[1];         /* slab_high_y */
        sb.params2[1] = uniform.params1[1];         /* weather_inv_scale */
        sb.params2[2] = uniform.params1[2];         /* noise_inv_scale */
        sb.params2[3] = uniform.params0[3];         /* time */
        sb.params3[0] = uniform.params2[0];         /* wind_x */
        sb.params3[1] = uniform.params2[1];         /* wind_z */
        sb.params3[2] = uniform.params1[0];         /* coverage_bias */
        /* Optical depth scale: scales with clouds->density so denser clouds
         * cast deeper shadows (thin cirrus = faint shadow, thick cumulus =
         * dark shadow). 80 is an empirical slab-integration constant that
         * makes the default density (0.05) land at a reasonable shadow
         * intensity without overwhelming the scene. */
        sb.params3[3] = clouds->density * 80.0f;
        wgpuQueueWriteBuffer(engine->queue, impl->shadow_uniform_buffer, 0,
            &sb, sizeof(sb));

        flecs_clouds_publishShadow(
            (FlecsEngineImpl*)engine, impl->shadow_view,
            origin_x, origin_z, impl->shadow_inv_footprint,
            clouds->shadow_strength);
    }

    /* Dynamic coverage: evolve the weather texture with a simple cellular
     * automaton (advection + diffusion + FBM forcing). Runs every ~200 ms
     * on CPU; cost is trivial at 256^2. A coverage change snaps the bake
     * forward by re-seeding from the FBM target. */
    bool coverage_changed =
        fabsf(clouds->coverage - impl->weather_last_coverage) > 1e-3f;
    double dt_since_update = impl->time_seconds - impl->weather_last_update;
    if (impl->weather_cpu &&
        (dt_since_update > 0.2 || coverage_changed))
    {
        if (coverage_changed) {
            flecs_clouds_bakeWeather(impl->weather_cpu,
                clouds->coverage, (float)impl->time_seconds);
        } else {
            flecs_clouds_evolveWeather(impl->weather_cpu,
                clouds->coverage, (float)impl->time_seconds,
                (float)dt_since_update,
                clouds->wind_x, clouds->wind_z);
        }
        WGPUTexelCopyTextureInfo dst = {
            .texture = impl->weather_texture,
            .mipLevel = 0,
            .origin = { 0, 0, 0 },
            .aspect = WGPUTextureAspect_All
        };
        WGPUTexelCopyBufferLayout layout = {
            .offset = 0,
            .bytesPerRow = FLECS_CLOUDS_WEATHER_SIZE * 4u,
            .rowsPerImage = FLECS_CLOUDS_WEATHER_SIZE
        };
        WGPUExtent3D extent = {
            FLECS_CLOUDS_WEATHER_SIZE, FLECS_CLOUDS_WEATHER_SIZE, 1 };
        wgpuQueueWriteTexture(engine->queue, &dst, impl->weather_cpu,
            (size_t)FLECS_CLOUDS_WEATHER_SIZE * FLECS_CLOUDS_WEATHER_SIZE * 4u,
            &layout, &extent);
        impl->weather_last_update = impl->time_seconds;
        impl->weather_last_coverage = clouds->coverage;
    }

    return true;
}

static bool flecsEngine_clouds_bind(
    const ecs_world_t *world,
    const FlecsEngineImpl *engine,
    const FlecsRenderViewImpl *view_impl,
    ecs_entity_t effect_entity,
    const FlecsRenderEffect *effect,
    const FlecsRenderEffectImpl *effect_impl,
    WGPUBindGroupEntry *entries,
    uint32_t *entry_count)
{
    (void)effect;
    (void)effect_impl;
    (void)engine;

    if (!view_impl || !view_impl->depth_texture_view) return false;

    FlecsCloudsImpl *impl = ecs_get(
        world, effect_entity, FlecsCloudsImpl);
    if (!impl || !impl->uniform_buffer) return false;

    /* State (uniforms, publish, CA) was updated in the render_callback
     * before this point; bind_callback only fills bind-group entries. */

    entries[2] = (WGPUBindGroupEntry){
        .binding = 2, .textureView = view_impl->depth_texture_view };
    entries[3] = (WGPUBindGroupEntry){
        .binding = 3, .buffer = impl->uniform_buffer,
        .offset = 0, .size = sizeof(FlecsCloudsUniform) };
    entries[4] = (WGPUBindGroupEntry){
        .binding = 4, .textureView = impl->weather_view };
    entries[5] = (WGPUBindGroupEntry){
        .binding = 5, .textureView = impl->noise_view };
    entries[6] = (WGPUBindGroupEntry){
        .binding = 6, .sampler = impl->repeat_sampler };

    *entry_count = 7;
    return true;
}

/* Custom render callback: bake the cloud shadow texture, then run the
 * standard cloud composite. The bake fragment shader runs at 256x256 and
 * computes per-ground-point cloud transmittance from the same density
 * formula the sky shader uses, so PBR can sample shadow values directly by
 * world XZ. */
static bool flecsEngine_clouds_render(
    const ecs_world_t *world,
    FlecsEngineImpl *engine,
    const FlecsRenderViewImpl *view_impl,
    WGPUCommandEncoder encoder,
    ecs_entity_t effect_entity,
    const FlecsRenderEffect *effect,
    FlecsRenderEffectImpl *effect_impl,
    WGPUTextureView input_view,
    WGPUTextureFormat input_format,
    WGPUTextureView output_view,
    WGPUTextureFormat output_format,
    WGPULoadOp output_load_op)
{
    (void)input_format;

    FlecsCloudsImpl *impl = ecs_get_mut(
        (ecs_world_t*)world, effect_entity, FlecsCloudsImpl);
    if (!impl || !impl->shadow_bake_pipeline ||
        !impl->shadow_bake_bind_group || !impl->shadow_view)
    {
        return false;
    }

    const FlecsClouds *clouds = ecs_get(world, effect_entity, FlecsClouds);
    if (!clouds) return false;

    /* Update uniforms, publish shadow params, run the weather CA. Done here
     * rather than in bind_callback so the bake pass below reads fresh data. */
    if (!flecs_clouds_updateState(world, engine, effect_entity, impl, clouds)) {
        return false;
    }

    /* Bake the shadow texture for THIS frame's PBR/composite. */
    {
        WGPURenderPassColorAttachment att = {
            .view = impl->shadow_view,
            WGPU_DEPTH_SLICE
            .loadOp = WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = (WGPUColor){ 1.0, 1.0, 1.0, 1.0 }
        };
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
            encoder, &(WGPURenderPassDescriptor){
                .colorAttachmentCount = 1,
                .colorAttachments = &att
            });
        if (!pass) return false;
        wgpuRenderPassEncoderSetPipeline(pass, impl->shadow_bake_pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0,
            impl->shadow_bake_bind_group, 0, NULL);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    return flecsEngine_renderEffect_render(
        world, engine, view_impl, encoder,
        output_view, output_load_op, (WGPUColor){0, 0, 0, 1},
        effect_entity, effect, effect_impl,
        input_view, output_format,
        "Clouds", NULL);
}

FlecsClouds flecsEngine_cloudsSettingsDefault(void)
{
    return (FlecsClouds){
        .atmosphere = 0,
        .low_altitude_km = 1.5f,
        .high_altitude_km = 4.0f,
        .coverage = 0.55f,
        .density = 0.05f,
        .wind_x = 8.0f,
        .wind_z = 4.0f,
        .time_scale = 1.0f,
        .weather_scale_km = 40.0f,
        .noise_scale_km = 4.0f,
        .shadow_strength = 0.7f,
        /* M4 default footprint of 4 km centered around the camera. The 256x256
         * baked shadow texture covers this area giving ~16 m per shadow texel,
         * which is fine for cumulus-shadow detail. Cloud silhouettes overhead
         * align with their shadows on the ground because the bake uses the
         * same density formula as the sky shader. */
        .shadow_scale_km = 4.0f,
        .ambient_top = {220, 230, 255, 255},
        .ambient_bottom = {110, 120, 140, 255}
    };
}

ecs_entity_t flecsEngine_createEffect_clouds(
    ecs_world_t *world,
    ecs_entity_t parent,
    const char *name,
    int32_t input,
    const FlecsClouds *settings)
{
    ecs_entity_t effect = ecs_entity(world, { .parent = parent, .name = name });

    FlecsClouds c = settings ? *settings : flecsEngine_cloudsSettingsDefault();
    ecs_set_ptr(world, effect, FlecsClouds, &c);
    ecs_set(world, effect, FlecsRenderEffect, {
        .shader = flecsEngine_clouds_shader(world),
        .input = input,
        .setup_callback = flecsEngine_clouds_setup,
        .bind_callback = flecsEngine_clouds_bind,
        .render_callback = flecsEngine_clouds_render
    });

    return effect;
}

void flecsEngine_clouds_register(
    ecs_world_t *world)
{
    ECS_COMPONENT_DEFINE(world, FlecsClouds);
    ECS_COMPONENT_DEFINE(world, FlecsCloudsImpl);

    ecs_set_hooks(world, FlecsCloudsImpl, {
        .ctor = flecs_default_ctor,
        .move = ecs_move(FlecsCloudsImpl),
        .dtor = ecs_dtor(FlecsCloudsImpl)
    });

    ecs_struct(world, {
        .entity = ecs_id(FlecsClouds),
        .members = {
            { .name = "atmosphere", .type = ecs_id(ecs_entity_t) },
            { .name = "low_altitude_km", .type = ecs_id(ecs_f32_t) },
            { .name = "high_altitude_km", .type = ecs_id(ecs_f32_t) },
            { .name = "coverage", .type = ecs_id(ecs_f32_t) },
            { .name = "density", .type = ecs_id(ecs_f32_t) },
            { .name = "wind_x", .type = ecs_id(ecs_f32_t) },
            { .name = "wind_z", .type = ecs_id(ecs_f32_t) },
            { .name = "time_scale", .type = ecs_id(ecs_f32_t) },
            { .name = "weather_scale_km", .type = ecs_id(ecs_f32_t) },
            { .name = "noise_scale_km", .type = ecs_id(ecs_f32_t) },
            { .name = "shadow_strength", .type = ecs_id(ecs_f32_t) },
            { .name = "shadow_scale_km", .type = ecs_id(ecs_f32_t) },
            { .name = "ambient_top", .type = ecs_id(flecs_rgba_t) },
            { .name = "ambient_bottom", .type = ecs_id(flecs_rgba_t) }
        }
    });
}
