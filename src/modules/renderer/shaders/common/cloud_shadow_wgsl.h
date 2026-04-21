#ifndef FLECS_ENGINE_SHADER_COMMON_CLOUD_SHADOW_WGSL_H
#define FLECS_ENGINE_SHADER_COMMON_CLOUD_SHADOW_WGSL_H

/* M4: sample the baked cloud shadow texture (Hillaire 2016 §5.9). The
 * shadow texture stores per-ground-point cloud transmittance in a world-
 * aligned footprint around the camera, so PBR just maps world XZ to
 * shadow UV and reads the value. cloud_shadow_params packs:
 *   x = footprint origin world X (lower-left corner)
 *   y = footprint origin world Z
 *   z = strength in [0,1] (0 disables)
 *   w = 1 / footprint size (world units)
 * Outside the footprint, returns 1.0 (no shadow). */

#define FLECS_ENGINE_SHADER_COMMON_CLOUD_SHADOW_WGSL \
    "@group(0) @binding(11) var cloud_shadow_tex : texture_2d<f32>;\n" \
    "fn computeCloudShadow(world_pos : vec3<f32>) -> f32 {\n" \
    "  let strength = uniforms.cloud_shadow_params.z;\n" \
    "  if (strength <= 0.0) { return 1.0; }\n" \
    "  let origin = uniforms.cloud_shadow_params.xy;\n" \
    "  let inv_footprint = uniforms.cloud_shadow_params.w;\n" \
    "  let uv = (world_pos.xz - origin) * inv_footprint;\n" \
    "  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n" \
    "    return 1.0;\n" \
    "  }\n" \
    "  let trans = textureSampleLevel(\n" \
    "    cloud_shadow_tex, ibl_sampler, uv, 0.0).r;\n" \
    "  return mix(1.0, trans, strength);\n" \
    "}\n"

#endif
