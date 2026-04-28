#ifndef FLECS_ENGINE_CLOUDS_H
#define FLECS_ENGINE_CLOUDS_H

ECS_STRUCT(FlecsClouds, {
    ecs_entity_t atmosphere;     /* atmosphere entity (for sun-direction lookup) */
    float low_altitude_km;       /* cloud slab bottom, world altitude in km */
    float high_altitude_km;      /* cloud slab top, world altitude in km */
    float coverage;              /* 0..1 global coverage bias */
    float cloud_type_bias;       /* -0.5..0.5 shifts type toward stratus / cumulonimbus */
    float density;               /* extinction scale */
    float wind_x;                /* world units per second */
    float wind_z;
    float weather_scale_km;      /* world km per weather-texture tile */
    float noise_scale_km;        /* world km per noise-texture tile */
    /* Ground cloud-shadow controls. shadow_strength=0 disables. */
    float shadow_strength;
    float shadow_scale_km;       /* world km per shadow-projection tile */
    /* Baked cloud-shadow texture resolution (square). Applied when the
     * effect is created; changing at runtime has no effect. */
    int32_t shadow_size;
    flecs_rgba_t ambient_top;
    flecs_rgba_t ambient_bottom;
    /* Resolution divisor for cloud raymarching. 1 = full-res, 2 = half-res
     * in each axis (~4x faster), 4 = quarter-res (~16x faster). Foreground
     * pixels are always taken from the full-res input so geometry edges stay
     * sharp; only sky pixels pay the low-res cost. Values < 1 are clamped
     * to 1. */
    float render_scale;
});

extern ECS_COMPONENT_DECLARE(FlecsClouds);

FlecsClouds flecsEngine_cloudsSettingsDefault(void);

ecs_entity_t flecsEngine_createEffect_clouds(
    ecs_world_t *world,
    ecs_entity_t parent,
    const char *name,
    int32_t input,
    const FlecsClouds *settings);

void flecsEngine_clouds_register(
    ecs_world_t *world);

#endif
