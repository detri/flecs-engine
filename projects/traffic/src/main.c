#include "traffic.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

typedef struct {
    const char *frame_output_path;
    const char *scene_path;
    int32_t width;
    int32_t height;
} TrafficAppOptions;

static int trafficParseArgs(
    int argc,
    char *argv[],
    TrafficAppOptions *options)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "--scene") && i + 1 < argc) {
            options->scene_path = argv[++i];
            continue;
        }
        if (!strcmp(arg, "--frame-out") && i + 1 < argc) {
            options->frame_output_path = argv[++i];
            continue;
        }
        if (!strcmp(arg, "--width") && i + 1 < argc) {
            options->width = atoi(argv[++i]);
            continue;
        }
        if (!strcmp(arg, "--height") && i + 1 < argc) {
            options->height = atoi(argv[++i]);
            continue;
        }
    }

    return 0;
}

static float randf(int n) {
    return (float)(rand() % n);
}

static ecs_entity_t trafficPickCarPrefab(ecs_world_t *world) {
    static const char *names[] = {
        "kenney.cars.sedan",
        "kenney.cars.sedan_sports",
        "kenney.cars.hatchback_sports",
        "kenney.cars.suv",
        "kenney.cars.suv_luxury",
        "kenney.cars.taxi",
        "kenney.cars.police",
        "kenney.cars.ambulance",
        "kenney.cars.delivery",
        "kenney.cars.van",
        "kenney.cars.race",
        "kenney.cars.race_future",
        "kenney.cars.firetruck",
        "kenney.cars.garbage_truck",
        "kenney.cars.truck",
        "kenney.cars.truck_flat"
    };
    int n = sizeof(names) / sizeof(names[0]);
    int pick = rand() % n;
    return ecs_lookup(world, names[pick]);
}

static void trafficPopulateLanes(ecs_world_t *world) {
    ecs_query_t *q = ecs_query(world, {
        .terms = {
            {.id = ecs_id(TrafficLane), .inout = EcsIn},
            {.id = ecs_id(TrafficCorner), .src.id = EcsSelf,
                .oper = EcsNot}
        }
    });

    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; i++) {
            ecs_entity_t lane = it.entities[i];

            if (trafficCars_intersectionFromLane(world, lane)) {
                continue;
            }

            ecs_entity_t prefab = trafficPickCarPrefab(world);
            if (!prefab) continue;

            ecs_entity_t car = ecs_new_w_pair(world, EcsChildOf, TrafficCarRoot);
            ecs_add_pair(world, car, EcsIsA, prefab);
            ecs_set(world, car, FlecsPosition3, {0, 0, 0});
            ecs_set(world, car, FlecsRotation3, {0, 0, 0});
            ecs_set(world, car, FlecsScale3, {1.0f, 1.0f, 1.0f});

            float position = randf(20);
            trafficCars_addCarToLane(world, lane, car,
                position, 0, 0,
                TrafficCarStateUnknown, TrafficEolDefault, 0);
        }
    }

    ecs_query_fini(q);
}

#ifdef __EMSCRIPTEN__
static void trafficWasmFrame(void *arg) {
    ecs_world_t *world = arg;
    if (!ecs_progress(world, 0)) {
        emscripten_cancel_main_loop();
    }
}
#endif

int main(int argc, char *argv[]) {
    TrafficAppOptions options = {
        .width = 1280,
        .height = 800
    };

    trafficParseArgs(argc, argv, &options);

    ecs_world_t *world = ecs_init();
#ifndef __EMSCRIPTEN__
    ECS_IMPORT(world, FlecsStats);
#endif
    ECS_IMPORT(world, FlecsScriptMath);
    ECS_IMPORT(world, FlecsEngine);

    ECS_IMPORT(world, TrafficCars);

    if (!options.frame_output_path) {
        ecs_log_set_level(0);
    }

    ecs_entity_t surface = ecs_entity(world, { .name = "surface" });
    ecs_set(world, surface, FlecsSurface, {
        .title = "Flecs Traffic",
        .width = options.width,
        .height = options.height,
        .resolution_scale = 1,
        .vsync = true,
        .msaa = FlecsMsaa4x,
        .write_to_file = options.frame_output_path,
    });

    const char *scene = options.scene_path ?
        options.scene_path : "etc/scenes/traffic.flecs";
    ecs_entity_t s = ecs_script(world, { .filename = scene });
    if (!s) {
        ecs_err("failed to load scene %s", scene);
    }

    trafficCars_finalizeScene(world);
    trafficPopulateLanes(world);

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(
        (em_arg_callback_func)trafficWasmFrame, world, 0, 1);
#else
    if (!options.frame_output_path) {
        ecs_singleton_set(world, EcsRest, {0});
    }
    while (ecs_progress(world, 0)) {}
#endif

    ecs_log_set_level(-1);
    return ecs_fini(world);
}
