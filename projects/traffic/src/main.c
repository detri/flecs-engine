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
    for (int i = 1; i < argc; i ++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "--scene") && i + 1 < argc) {
            options->scene_path = argv[++ i];
            continue;
        }
        if (!strcmp(arg, "--frame-out") && i + 1 < argc) {
            options->frame_output_path = argv[++ i];
            continue;
        }
        if (!strcmp(arg, "--width") && i + 1 < argc) {
            options->width = atoi(argv[++ i]);
            continue;
        }
        if (!strcmp(arg, "--height") && i + 1 < argc) {
            options->height = atoi(argv[++ i]);
            continue;
        }
    }
    return 0;
}

static float randf(int n) {
    return (float)(rand() % n);
}

static void trafficDeleteEmptyTables(ecs_iter_t *it) {
    ecs_delete_empty_tables_desc_t desc = {0};
    desc.delete_generation = 1;
    ecs_delete_empty_tables(it->world, &desc);
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

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "DeleteEmptyTables",
            .add = ecs_ids(ecs_dependson(EcsPostLoad))
        }),
        .interval = 10,
        .immediate = true,
        .callback = trafficDeleteEmptyTables
    });

    const char *scene = options.scene_path ?
        options.scene_path : "etc/scenes/traffic.flecs";
    if (!ecs_script(world, { .filename = scene })) {
        ecs_err("failed to load scene %s", scene);
    }

    /* Resolve Kenney car prefabs once. */
    static const char *car_prefab_names[] = {
        "kenney.cars.sedan",
        "kenney.cars.sedan_sports",
        "kenney.cars.hatchback_sports",
        "kenney.cars.suv",
        "kenney.cars.suv_luxury",
        "kenney.cars.taxi",
        "kenney.cars.police",
        "kenney.cars.ambulance",
        "kenney.cars.delivery",
        "kenney.cars.delivery_flat",
        "kenney.cars.van",
        "kenney.cars.race",
        "kenney.cars.race_future",
        "kenney.cars.firetruck",
        "kenney.cars.garbage_truck",
        "kenney.cars.truck",
        "kenney.cars.truck_flat"
    };
    int car_prefab_count = sizeof(car_prefab_names) / sizeof(car_prefab_names[0]);
    ecs_entity_t car_prefabs[sizeof(car_prefab_names) / sizeof(car_prefab_names[0])];
    int resolved_prefabs = 0;
    for (int p = 0; p < car_prefab_count; p ++) {
        ecs_entity_t prefab = ecs_lookup(world, car_prefab_names[p]);
        if (prefab) {
            car_prefabs[resolved_prefabs ++] = prefab;
        }
    }

    /* Populate lanes with cars. */
    ecs_query_t *lane_q = ecs_query(world, {
        .terms = {
            { .id = ecs_id(TrafficLane), .inout = EcsIn },
            { .id = ecs_id(TrafficCorner), .src.id = EcsSelf,
              .oper = EcsNot }
        }
    });

    ecs_iter_t lit = ecs_query_iter(world, lane_q);
    while (ecs_query_next(&lit)) {
        for (int i = 0; i < lit.count; i ++) {
            ecs_entity_t lane = lit.entities[i];

            if (trafficCars_intersectionFromLane(world, lane)) {
                continue;
            }

            for (int n = 1; n >= 0; n --) {
                ecs_entity_t car = ecs_new_w_pair(world, EcsChildOf,
                    TrafficCarRoot);
                ecs_set(world, car, TrafficCar, {0});
                if (resolved_prefabs) {
                    ecs_add_pair(world, car, EcsIsA,
                        car_prefabs[rand() % resolved_prefabs]);
                } else {
                    ecs_set(world, car, FlecsBox, {3, 1, 1});
                }

                trafficCars_addCarToLane(world,
                    0, lane, car,
                    n * 8 + randf(5),
                    0, 0,
                    TrafficCarStateUnknown,
                    TrafficCarEolDefault, 0);
            }
        }
    }
    ecs_query_fini(lane_q);

    ecs_set_threads(world, 8);

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
