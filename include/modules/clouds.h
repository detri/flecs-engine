#ifndef FLECS_ENGINE_CLOUDS_H
#define FLECS_ENGINE_CLOUDS_H

ECS_STRUCT(FlecsClouds, {
    ecs_entity_t atmosphere;     /* atmosphere entity (for sun-direction lookup) */
    float low_altitude_km;       /* cloud slab bottom, world altitude in km */
    float high_altitude_km;      /* cloud slab top, world altitude in km */
    float coverage;              /* 0..1 global coverage bias */
    float density;               /* extinction scale */
    float wind_x;                /* world units per second */
    float wind_z;
    /* Global time multiplier for all cloud progression (sky cloud scroll,
     * weather CA, wind advection). 1.0 = real time, 0.0 = frozen, 2.0 =
     * 2x faster. Independent of wind_x/wind_z which control drift
     * direction and base speed. */
    float time_scale;
    float weather_scale_km;      /* world km per weather-texture tile */
    float noise_scale_km;        /* world km per noise-texture tile */
    /* Ground cloud-shadow controls. shadow_strength=0 disables. */
    float shadow_strength;
    float shadow_scale_km;       /* world km per shadow-projection tile */
    flecs_rgba_t ambient_top;
    flecs_rgba_t ambient_bottom;
    /* Resolution scale for cloud raymarching. 1.0 = full-res, 0.5 = half-res
     * (~4x faster), 0.25 = quarter-res. Foreground pixels are always taken
     * from the full-res input so geometry edges stay sharp; only sky pixels
     * pay the low-res cost. Values outside (0, 1] are clamped to 1.0. */
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
