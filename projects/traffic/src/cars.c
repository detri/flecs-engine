#include "traffic.h"

#include <math.h>
#include <string.h>
#include <assert.h>

ECS_COMPONENT_DECLARE(TrafficCar);
ECS_COMPONENT_DECLARE(TrafficLaneCars);
ECS_COMPONENT_DECLARE(TrafficLaneCarEntities);
ECS_COMPONENT_DECLARE(TrafficLane);
ECS_COMPONENT_DECLARE(TrafficCorner);
ECS_COMPONENT_DECLARE(TrafficRoadConnect);
ECS_COMPONENT_DECLARE(TrafficRoad);
ECS_COMPONENT_DECLARE(TrafficRoadLanes);
ECS_COMPONENT_DECLARE(TrafficIntersection);
ECS_COMPONENT_DECLARE(TrafficIntersectionRoads);
ECS_COMPONENT_DECLARE(TrafficIntersectionMovement);

ECS_DECLARE(TrafficCarRoot);
ECS_DECLARE(TrafficRoadRoot);

#define TRAFFIC_CAR_VISUAL_SCALE 3.0f

static const float TrafficAccelerationForce = 1.5f;
static const float TrafficBreakForce = 5.0f;
static const float TrafficHardBreakForce = 12.0f;
static const uint8_t TrafficMaxWaitCount = 60;
static const float TrafficCarMass = 10.0f;
static const float TrafficCarLength = 3.0f;

static int LaneTick = 0;

static float trafficCars_minDistanceForSpeed(float s1) {
    if (s1 < 0.1f) {
        s1 = 0.1f;
    }
    return s1 * 50.0f;
}

static uint8_t trafficCars_intersectionReserve(TrafficIntersectionRoads *ir) {
    return ir->next_reservation++;
}

static void trafficCars_intersectionRelease(
    TrafficIntersectionRoads *ir,
    uint8_t reservation)
{
    (void)reservation;
    ir->current_reservation++;
}

static ecs_entity_t trafficCars_findNextLane(const TrafficIntersectionMovement *im) {
    int8_t turn = rand() % 3;

    if (!im->lanes[turn]) {
        int i;
        for (i = 1; i < 3; i++) {
            if (im->lanes[(turn + i) % 3]) {
                break;
            }
        }
        if (i == 3) {
            ecs_err("intersection without lanes");
            return 0;
        }
        turn = (turn + i) % 3;
    }

    return im->lanes[turn];
}

static float trafficCars_spaceInLane(
    ecs_world_t *world,
    ecs_entity_t lane)
{
    if (!lane || !ecs_is_alive(world, lane)) return 0;
    const TrafficLaneCars *cars = ecs_get(world, lane, TrafficLaneCars);
    if (!cars || !cars->count) {
        const TrafficLane *l = ecs_get(world, lane, TrafficLane);
        return l ? l->length : 0.0f;
    }

    const TrafficCar *last_car = &cars->cars[cars->count - 1];
    return last_car->position - last_car->length;
}

static float trafficCars_spaceInDestinationLane(
    ecs_world_t *world,
    ecs_entity_t next_lane)
{
    if (!next_lane || !ecs_is_alive(world, next_lane)) return 0;
    const TrafficLane *l = ecs_get(world, next_lane, TrafficLane);
    if (!l || !l->next) {
        return 0;
    }
    return trafficCars_spaceInLane(world, l->next);
}

static bool trafficCars_carFitsInDestination(
    ecs_world_t *world,
    ecs_entity_t next_lane,
    TrafficCar *car)
{
    if (!next_lane || !ecs_is_alive(world, next_lane)) return false;
    float space = trafficCars_spaceInDestinationLane(world, next_lane);

    const TrafficLaneCars *cars = ecs_get(world, next_lane, TrafficLaneCars);
    if (cars) {
        for (int i = 0; i < cars->count; i++) {
            space -= cars->cars[i].length;
        }
        space -= cars->count * 2;
    }

    return space > car->length;
}

static void trafficCars_waitForLane(
    TrafficCar *car,
    const TrafficIntersectionMovement *im)
{
    car->wait_count++;
    if (car->wait_count > TrafficMaxWaitCount) {
        car->next_lane = trafficCars_findNextLane(im);
        car->wait_count = 0;
    }
}

void trafficCars_addCarToLane(
    ecs_world_t *world,
    ecs_entity_t out_lane,
    ecs_entity_t car_entity,
    float position,
    float speed,
    float target_speed,
    uint8_t state,
    uint8_t eol_state,
    uint8_t reservation)
{
    if (!out_lane) {
        ecs_err("no lane to move car %u to", (uint32_t)car_entity);
        return;
    }

    TrafficLaneCars *cars = ecs_get_mut(world, out_lane, TrafficLaneCars);
    if (!cars) {
        ecs_err("lane missing TrafficLaneCars");
        return;
    }

    if (cars->count >= TRAFFIC_MAX_CARS_PER_LANE) {
        ecs_err("lane full, can't add car %u", (uint32_t)car_entity);
        return;
    }

    if (cars->count) {
        TrafficCar *last_car = &cars->cars[cars->count - 1];
        if ((last_car->position - last_car->length) < position) {
            ecs_err("adding car %u would cause crash", (uint32_t)car_entity);
            position = 0;
            speed = 0;
            target_speed = 0;
        }
    }

    TrafficCar *car = &cars->cars[cars->count];
    car->position = position;
    car->speed = speed;
    car->target_speed = target_speed;
    car->state = state;
    car->eol_state = eol_state;
    car->next_lane = out_lane;
    car->mass = TrafficCarMass;
    car->length = TrafficCarLength;
    car->reservation = reservation;
    car->wait_count = 0;

    TrafficLaneCarEntities *car_entities = ecs_get_mut(
        world, out_lane, TrafficLaneCarEntities);
    car_entities->cars[cars->count] = car_entity;

    cars->count++;
}

ecs_entity_t trafficCars_intersectionFromLane(
    ecs_world_t *world,
    ecs_entity_t lane)
{
    const TrafficLane *l = ecs_get(world, lane, TrafficLane);
    if (!l || !l->road) return 0;
    return ecs_get_parent(world, l->road);
}

static void trafficCars_onSetCorner(ecs_iter_t *it) {
    TrafficCorner *c = ecs_field(it, TrafficCorner, 0);
    for (int i = 0; i < it->count; i++) {
        TrafficLane *l = ecs_get_mut(it->world, it->entities[i], TrafficLane);
        if (l) {
            l->length = c[i].radius * (float)GLM_PI * 0.5f;
        }
    }
}

static void trafficCars_createLanesForRoad(
    ecs_world_t *world,
    ecs_entity_t e,
    const TrafficRoad *r,
    TrafficRoadLanes *rl)
{
    if (rl->lanes[0] || rl->lanes[1]) return;

    ecs_entity_t left = ecs_new_w_pair(world, EcsChildOf, e);
    ecs_entity_t right = ecs_new_w_pair(world, EcsChildOf, e);

    if (!r->corner) {
        ecs_set(world, left, TrafficLane,
            {r->length, r->lane_width, r->max_speed, 0, e});
        ecs_set(world, left, FlecsPosition3, {0, 0, r->lane_width / 2});
        ecs_set(world, left, FlecsRotation3, {0, (float)GLM_PI, 0});

        ecs_set(world, right, TrafficLane,
            {r->length, r->lane_width, r->max_speed, 0, e});
        ecs_set(world, right, FlecsPosition3, {0, 0, -r->lane_width / 2});
    } else {
        float left_radius = r->lane_width * 1.5f;
        float left_speed = left_radius / 50.0f;
        if (left_speed > r->max_speed) left_speed = r->max_speed;

        ecs_set(world, left, TrafficLane,
            {r->length, r->lane_width, left_speed, 0, e});
        ecs_set(world, left, FlecsPosition3,
            {r->lane_width / 2, 0, r->lane_width / 2});
        ecs_set(world, left, TrafficCorner, {left_radius, true});

        float right_radius = r->lane_width / 2.0f;
        float right_speed = right_radius / 30.0f;
        if (right_speed > r->max_speed) right_speed = r->max_speed;

        ecs_set(world, right, TrafficLane,
            {r->length, r->lane_width, right_speed, 0, e});
        ecs_set(world, right, FlecsPosition3,
            {-r->lane_width / 2, 0, -r->lane_width / 2});
        ecs_set(world, right, TrafficCorner, {right_radius, false});
    }

    if (r->invert_corner) {
        rl->lanes[0] = right;
        rl->lanes[1] = left;
    } else {
        rl->lanes[0] = left;
        rl->lanes[1] = right;
    }
}

static void trafficCars_createAllLanes(ecs_world_t *world) {
    ecs_query_t *q = ecs_query(world, {
        .terms = {
            {.id = ecs_id(TrafficRoad), .inout = EcsIn},
            {.id = ecs_id(TrafficRoadLanes), .inout = EcsInOut}
        }
    });

    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        const TrafficRoad *roads = ecs_field(&it, TrafficRoad, 0);
        TrafficRoadLanes *rl = ecs_field(&it, TrafficRoadLanes, 1);
        for (int i = 0; i < it.count; i++) {
            trafficCars_createLanesForRoad(world,
                it.entities[i], &roads[i], &rl[i]);
        }
    }
    ecs_query_fini(q);
}

static void trafficCars_createIntersectionRoads(ecs_world_t *world) {
    ecs_query_t *q = ecs_query(world, {
        .terms = {
            {.id = ecs_id(TrafficIntersection), .inout = EcsIn},
            {.id = ecs_id(TrafficIntersectionRoads), .inout = EcsInOut}
        }
    });

    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        const TrafficIntersection *is = ecs_field(&it, TrafficIntersection, 0);
        TrafficIntersectionRoads *irs = ecs_field(&it, TrafficIntersectionRoads, 1);

        for (int idx = 0; idx < it.count; idx++) {
            ecs_entity_t e = it.entities[idx];
            const TrafficIntersection *i = &is[idx];
            TrafficIntersectionRoads *ir = &irs[idx];

            bool any = false;
            for (int k = 0; k < 6; k++) if (ir->roads[k]) any = true;
            if (any) continue;

            if (i->roads[TrafficDirTop].road && i->roads[TrafficDirBottom].road) {
                ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
                ecs_set(world, road, TrafficRoad,
                    {i->lane_width * 2, i->lane_width, i->max_speed, false, false, {0, 0}, e});
                ecs_set(world, road, FlecsPosition3, {0, 0, 0});
                ecs_set(world, road, FlecsRotation3, {0, (float)GLM_PI * 1.5f, 0});
                ir->roads[TrafficConnTopToBottom] = road;
            }
            if (i->roads[TrafficDirLeft].road && i->roads[TrafficDirRight].road) {
                ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
                ecs_set(world, road, TrafficRoad,
                    {i->lane_width * 2, i->lane_width, i->max_speed, false, false, {0, 0}, e});
                ecs_set(world, road, FlecsPosition3, {0, 0, 0});
                ecs_set(world, road, FlecsRotation3, {0, (float)GLM_PI, 0});
                ir->roads[TrafficConnLeftToRight] = road;
            }
            if (i->roads[TrafficDirLeft].road && i->roads[TrafficDirTop].road) {
                ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
                ecs_set(world, road, TrafficRoad,
                    {i->lane_width, i->lane_width, i->max_speed, true, true, {0, 0}, e});
                ecs_set(world, road, FlecsPosition3, {0, 0, 0});
                ecs_set(world, road, FlecsRotation3, {0, (float)GLM_PI / 2.0f, 0});
                ir->roads[TrafficConnTopToLeft] = road;
            }
            if (i->roads[TrafficDirRight].road && i->roads[TrafficDirTop].road) {
                ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
                ecs_set(world, road, TrafficRoad,
                    {i->lane_width, i->lane_width, i->max_speed, true, false, {0, 0}, e});
                ecs_set(world, road, FlecsPosition3, {0, 0, 0});
                ecs_set(world, road, FlecsRotation3, {0, (float)GLM_PI, 0});
                ir->roads[TrafficConnTopToRight] = road;
            }
            if (i->roads[TrafficDirRight].road && i->roads[TrafficDirBottom].road) {
                ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
                ecs_set(world, road, TrafficRoad,
                    {i->lane_width, i->lane_width, i->max_speed, true, true, {0, 0}, e});
                ecs_set(world, road, FlecsPosition3, {0, 0, 0});
                ecs_set(world, road, FlecsRotation3, {0, (float)GLM_PI * 1.5f, 0});
                ir->roads[TrafficConnBottomToRight] = road;
            }
            if (i->roads[TrafficDirLeft].road && i->roads[TrafficDirBottom].road) {
                ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
                ecs_set(world, road, TrafficRoad,
                    {i->lane_width, i->lane_width, i->max_speed, true, false, {0, 0}, e});
                ecs_set(world, road, FlecsPosition3, {0, 0, 0});
                ir->roads[TrafficConnBottomToLeft] = road;
            }
        }
    }
    ecs_query_fini(q);
}

static void trafficCars_connectRoads(ecs_world_t *world) {
    ecs_query_t *q = ecs_query(world, {
        .terms = {
            {.id = ecs_id(TrafficRoad), .inout = EcsIn},
            {.id = ecs_id(TrafficRoadLanes), .inout = EcsIn}
        }
    });

    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        const TrafficRoad *roads = ecs_field(&it, TrafficRoad, 0);
        const TrafficRoadLanes *rl = ecs_field(&it, TrafficRoadLanes, 1);

        for (int i = 0; i < it.count; i++) {
            const TrafficRoad *r = &roads[i];
            if (!r->next.road) continue;

            int8_t edge = r->next.edge;
            const TrafficRoadLanes *rl_next = ecs_get(
                world, r->next.road, TrafficRoadLanes);
            if (!rl_next) continue;

            TrafficLane *left = ecs_get_mut(
                world, rl_next->lanes[edge], TrafficLane);
            if (left) {
                left->next = rl[i].lanes[edge];
            }

            TrafficLane *right = ecs_get_mut(
                world, rl[i].lanes[1 - edge], TrafficLane);
            if (right) {
                right->next = rl_next->lanes[1 - edge];
            }
        }
    }

    ecs_query_fini(q);
}

static ecs_entity_t trafficCars_intersectionMovement(
    ecs_world_t *world,
    ecs_entity_t intersection,
    TrafficIntersectionRoads *ir,
    int dir,
    ecs_entity_t l0,
    ecs_entity_t l1,
    ecs_entity_t l2)
{
    ecs_entity_t lanes[3] = {l0, l1, l2};
    ecs_entity_t lane = 0;
    int count = 0;

    for (int i = 0; i < 3; i++) {
        if (lanes[i]) {
            lane = lanes[i];
            count++;
        }
    }

    if (count == 1) {
        return lane;
    }

    ecs_entity_t mv = ecs_new_w_pair(world, EcsChildOf, TrafficRoadRoot);
    ecs_set(world, mv, TrafficIntersectionMovement,
        {intersection, {l0, l1, l2}});

    ir->movements[dir] = mv;
    return mv;
}

static ecs_entity_t trafficCars_roadLane(
    ecs_world_t *world,
    const TrafficIntersection *i,
    int dir,
    bool flip)
{
    ecs_entity_t road = i->roads[dir].road;
    if (!road) return 0;
    int8_t lane = i->roads[dir].edge;
    if (flip) lane = 1 - lane;
    const TrafficRoadLanes *rl = ecs_get(world, road, TrafficRoadLanes);
    if (!rl) return 0;
    return rl->lanes[lane];
}

static ecs_entity_t trafficCars_intersectionLane(
    ecs_world_t *world,
    const TrafficIntersectionRoads *ir,
    int conn,
    int8_t lane)
{
    if (!ir->roads[conn]) return 0;
    const TrafficRoadLanes *rl = ecs_get(
        world, ir->roads[conn], TrafficRoadLanes);
    if (!rl) return 0;
    return rl->lanes[lane];
}

static void trafficCars_setLaneNext(
    ecs_world_t *world,
    ecs_entity_t lane,
    ecs_entity_t next)
{
    if (!lane || !ecs_is_alive(world, lane)) return;
    TrafficLane *l = ecs_get_mut(world, lane, TrafficLane);
    if (l) {
        l->next = next;
    }
}

static void trafficCars_connectIntersections(ecs_world_t *world) {
    ecs_query_t *q = ecs_query(world, {
        .terms = {
            {.id = ecs_id(TrafficIntersection), .inout = EcsIn},
            {.id = ecs_id(TrafficIntersectionRoads), .inout = EcsInOut}
        }
    });

    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        const TrafficIntersection *is = ecs_field(&it, TrafficIntersection, 0);
        TrafficIntersectionRoads *irs = ecs_field(&it, TrafficIntersectionRoads, 1);

        for (int idx = 0; idx < it.count; idx++) {
            ecs_entity_t e = it.entities[idx];
            const TrafficIntersection *i = &is[idx];
            TrafficIntersectionRoads *ir = &irs[idx];

            ecs_entity_t road_top    = i->roads[TrafficDirTop].road;
            ecs_entity_t road_right  = i->roads[TrafficDirRight].road;
            ecs_entity_t road_bottom = i->roads[TrafficDirBottom].road;
            ecs_entity_t road_left   = i->roads[TrafficDirLeft].road;

            if (road_top) {
                ecs_entity_t mv = trafficCars_intersectionMovement(world, e, ir, TrafficDirTop,
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToRight, 0),
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToBottom, 0),
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToLeft, 0));
                trafficCars_setLaneNext(world,
                    trafficCars_roadLane(world, i, TrafficDirTop, true), mv);
            }

            if (road_bottom) {
                ecs_entity_t mv = trafficCars_intersectionMovement(world, e, ir, TrafficDirBottom,
                    trafficCars_intersectionLane(world, ir, TrafficConnBottomToLeft, 0),
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToBottom, 1),
                    trafficCars_intersectionLane(world, ir, TrafficConnBottomToRight, 0));
                trafficCars_setLaneNext(world,
                    trafficCars_roadLane(world, i, TrafficDirBottom, true), mv);
            }

            if (road_left) {
                ecs_entity_t mv = trafficCars_intersectionMovement(world, e, ir, TrafficDirLeft,
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToLeft, 1),
                    trafficCars_intersectionLane(world, ir, TrafficConnLeftToRight, 0),
                    trafficCars_intersectionLane(world, ir, TrafficConnBottomToLeft, 1));
                trafficCars_setLaneNext(world,
                    trafficCars_roadLane(world, i, TrafficDirLeft, true), mv);
            }

            if (road_right) {
                ecs_entity_t mv = trafficCars_intersectionMovement(world, e, ir, TrafficDirRight,
                    trafficCars_intersectionLane(world, ir, TrafficConnBottomToRight, 1),
                    trafficCars_intersectionLane(world, ir, TrafficConnLeftToRight, 1),
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToRight, 1));
                trafficCars_setLaneNext(world,
                    trafficCars_roadLane(world, i, TrafficDirRight, true), mv);
            }

            if (road_top && road_bottom) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToBottom, 0),
                    trafficCars_roadLane(world, i, TrafficDirBottom, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToBottom, 1),
                    trafficCars_roadLane(world, i, TrafficDirTop, false));
            }

            if (road_left && road_right) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnLeftToRight, 0),
                    trafficCars_roadLane(world, i, TrafficDirRight, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnLeftToRight, 1),
                    trafficCars_roadLane(world, i, TrafficDirLeft, false));
            }

            if (road_top && road_right) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToRight, 0),
                    trafficCars_roadLane(world, i, TrafficDirRight, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToRight, 1),
                    trafficCars_roadLane(world, i, TrafficDirTop, false));
            }

            if (road_top && road_left) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToLeft, 0),
                    trafficCars_roadLane(world, i, TrafficDirLeft, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnTopToLeft, 1),
                    trafficCars_roadLane(world, i, TrafficDirTop, false));
            }

            if (road_bottom && road_right) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnBottomToRight, 0),
                    trafficCars_roadLane(world, i, TrafficDirRight, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnBottomToRight, 1),
                    trafficCars_roadLane(world, i, TrafficDirBottom, false));
            }

            if (road_bottom && road_left) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnBottomToLeft, 0),
                    trafficCars_roadLane(world, i, TrafficDirLeft, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficConnBottomToLeft, 1),
                    trafficCars_roadLane(world, i, TrafficDirBottom, false));
            }
        }
    }

    ecs_query_fini(q);
}

void trafficCars_finalizeScene(ecs_world_t *world) {
    trafficCars_createIntersectionRoads(world);
    trafficCars_createAllLanes(world);
    trafficCars_connectRoads(world);
    trafficCars_connectIntersections(world);
}

static void trafficCars_incrementLaneTick(ecs_iter_t *it) {
    (void)it;
    LaneTick++;
    if (LaneTick == 8) {
        LaneTick = 0;
    }
}

static void trafficCars_progressCars(ecs_iter_t *it) {
    TrafficLaneCars *cars = ecs_field(it, TrafficLaneCars, 0);
    for (int i = 0; i < it->count; i++) {
        for (int j = 0; j < cars[i].count; j++) {
            cars[i].cars[j].position += cars[i].cars[j].speed;
        }
    }
}

static void trafficCars_setTargetSpeed(
    TrafficCar *car,
    float next_position,
    float next_speed,
    uint8_t next_state,
    float lane_max_speed)
{
    if (car->state == TrafficCarStateCrashed) {
        return;
    }

    float distance = next_position - car->position;
    float min_d = trafficCars_minDistanceForSpeed(car->speed);
    float target_speed = lane_max_speed;

    if (distance < 0) {
        car->speed = 0;
        car->state = TrafficCarStateCrashed;
        return;
    } else if (distance < min_d) {
        if (next_state == TrafficCarStateBreaking ||
            next_state == TrafficCarStateBreakingHard)
        {
            target_speed = 0;
        } else {
            target_speed = next_speed;
        }
    }

    if (target_speed > lane_max_speed) {
        target_speed = lane_max_speed;
    }

    if (car->eol_state == TrafficEolWaitForIntersection ||
        car->eol_state == TrafficEolReserveIntersection ||
        car->eol_state == TrafficEolWaitForProtectedIntersection)
    {
        target_speed = 0;
    }

    car->target_speed = target_speed;
}

static void trafficCars_laneSetTargetSpeed(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);

    for (int row = 0; row < it->count; row++) {
        const TrafficLane *lane = &lanes[row];
        TrafficLaneCars *cars = &lane_cars[row];

        if (!cars->count) continue;
        if (((uint64_t)it->entities[row] & 7) != (uint64_t)LaneTick) continue;

        TrafficCar *car = &cars->cars[0];
        ecs_entity_t next_lane = lane->next;
        if (next_lane && !ecs_is_alive(world, next_lane)) {
            next_lane = 0;
        }
        const TrafficLane *nl = next_lane ? ecs_get(world, next_lane, TrafficLane) : NULL;
        bool intersection = false;

        if (!nl && next_lane) {
            intersection = true;
            if (car->next_lane && ecs_is_alive(world, car->next_lane)) {
                next_lane = car->next_lane;
                nl = ecs_get(world, next_lane, TrafficLane);
            }
        }

        float next_position_offset = 0;
        const TrafficCar *next_car = NULL;

        if (nl) {
            const TrafficLaneCars *nlc = ecs_get(world, next_lane, TrafficLaneCars);
            if (nlc && nlc->count) {
                next_car = &nlc->cars[nlc->count - 1];
            } else if (intersection) {
                ecs_entity_t nn = nl->next;
                if (nn && !ecs_is_alive(world, nn)) nn = 0;
                const TrafficLaneCars *nnlc = nn ?
                    ecs_get(world, nn, TrafficLaneCars) : NULL;
                if (nnlc && nnlc->count) {
                    next_car = &nnlc->cars[nnlc->count - 1];
                    next_position_offset = nl->length;
                }
            }
        }

        if (next_car) {
            float next_position = next_car->position - next_car->length / 2;
            next_position += next_position_offset;
            next_position += lane->length;
            trafficCars_setTargetSpeed(car, next_position,
                next_car->speed, next_car->state, lane->max_speed);
        } else {
            trafficCars_setTargetSpeed(car, 1000000.0f, 1000000.0f,
                TrafficCarStateDriving, lane->max_speed);
        }

        if (nl) {
            float min_d = trafficCars_minDistanceForSpeed(car->speed);
            if (lane->length - car->position < min_d) {
                if (car->target_speed > nl->max_speed) {
                    car->target_speed = nl->max_speed;
                }
            }
        }

        for (int i = 1; i < cars->count; i++) {
            TrafficCar *c = &cars->cars[i];
            TrafficCar *next = &cars->cars[i - 1];
            float next_position = next->position - next->length;
            trafficCars_setTargetSpeed(c, next_position,
                next->speed, next->state, lane->max_speed);
        }
    }
}

static void trafficCars_laneInitiateEol(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);

    for (int row = 0; row < it->count; row++) {
        if (((uint64_t)it->entities[row] & 7) != (uint64_t)LaneTick) continue;

        const TrafficLane *lane = &lanes[row];
        TrafficLaneCars *cars = &lane_cars[row];
        if (!cars->count) continue;

        TrafficCar *car = &cars->cars[0];
        if (car->eol_state != TrafficEolDefault) continue;

        float min_d = trafficCars_minDistanceForSpeed(car->speed);
        if (min_d < 1) min_d = 1;
        if ((lane->length - car->position) > min_d) continue;

        if (lane->next) {
            const TrafficLane *next_lane = ecs_get(world, lane->next, TrafficLane);
            if (!next_lane) {
                car->eol_state = TrafficEolReserveIntersection;
                car->target_speed = 0;
            }
        }
    }
}

static void trafficCars_laneSetEolState(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);

    for (int row = 0; row < it->count; row++) {
        if (((uint64_t)it->entities[row] & 7) != (uint64_t)LaneTick) continue;

        const TrafficLane *lane = &lanes[row];
        TrafficLaneCars *cars = &lane_cars[row];
        if (!cars->count) continue;

        TrafficCar *car = &cars->cars[0];

        switch (car->eol_state) {
        case TrafficEolDefault:
            car->wait_count = 0;
            continue;
        case TrafficEolMoveOnIntersection:
        case TrafficEolMoveOnProtectedIntersection:
        case TrafficEolOnIntersection:
        case TrafficEolOnProtectedIntersection:
            continue;
        default:
            break;
        }

        if (!lane->next) continue;
        const TrafficIntersectionMovement *im = ecs_get(
            world, lane->next, TrafficIntersectionMovement);
        if (!im) continue;

        TrafficIntersectionRoads *ir = ecs_get_mut(
            world, im->intersection, TrafficIntersectionRoads);
        if (!ir) continue;

        switch (car->eol_state) {
        case TrafficEolReserveIntersection:
            car->reservation = trafficCars_intersectionReserve(ir);
            car->next_lane = trafficCars_findNextLane(im);
            car->eol_state = TrafficEolWaitForIntersection;
            car->target_speed = 0;
            break;

        case TrafficEolWaitForIntersection:
            if (ir->current_reservation == car->reservation) {
                ecs_entity_t dst_lane = car->next_lane;
                if (dst_lane && trafficCars_carFitsInDestination(world, dst_lane, car)) {
                    car->eol_state = TrafficEolMoveOnIntersection;
                } else {
                    trafficCars_intersectionRelease(ir, car->reservation);
                    car->eol_state = TrafficEolReserveIntersection;
                }
            } else {
                trafficCars_waitForLane(car, im);
            }
            break;

        default:
            break;
        }
    }
}

static void trafficCars_laneSetDrivingState(ecs_iter_t *it) {
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 0);

    for (int row = 0; row < it->count; row++) {
        if (((uint64_t)it->entities[row] & 7) != (uint64_t)LaneTick) continue;

        TrafficLaneCars *cars = &lane_cars[row];
        for (int i = 0; i < cars->count; i++) {
            TrafficCar *car = &cars->cars[i];
            if (car->state == TrafficCarStateCrashed) continue;

            if (car->target_speed < car->speed) {
                car->state = TrafficCarStateBreaking;
            } else if (car->target_speed > car->speed) {
                car->state = TrafficCarStateAccelerating;
            } else {
                car->state = TrafficCarStateDriving;
            }

            if ((car->speed - car->target_speed) > 1.0f) {
                car->state = TrafficCarStateBreakingHard;
            }
        }
    }
}

static void trafficCars_laneAccelerate(ecs_iter_t *it) {
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 0);
    float dt = it->delta_system_time;
    float a = TrafficAccelerationForce * dt;
    float b = TrafficBreakForce * dt;
    float hb = TrafficHardBreakForce * dt;

    for (int row = 0; row < it->count; row++) {
        TrafficLaneCars *cars = &lane_cars[row];
        for (int i = 0; i < cars->count; i++) {
            TrafficCar *car = &cars->cars[i];
            if (car->state == TrafficCarStateCrashed) continue;

            switch (car->state) {
            case TrafficCarStateAccelerating:
                car->speed += a / car->mass;
                if (car->speed > car->target_speed) {
                    car->speed = car->target_speed;
                    car->state = TrafficCarStateDriving;
                }
                break;
            case TrafficCarStateBreaking:
                car->speed -= b / car->mass;
                if (car->speed < car->target_speed) {
                    car->speed = car->target_speed;
                    car->state = TrafficCarStateDriving;
                }
                break;
            case TrafficCarStateBreakingHard:
                car->speed -= hb / car->mass;
                if (car->speed < car->target_speed) {
                    car->speed = car->target_speed;
                    car->state = TrafficCarStateDriving;
                }
                break;
            default:
                break;
            }

            if (car->speed <= 0) {
                car->state = TrafficCarStateStopped;
                car->speed = 0;
            }
        }
    }
}

static void trafficCars_updateCarEntities(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    const TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);
    const TrafficLaneCarEntities *lane_ents = ecs_field(it, TrafficLaneCarEntities, 2);
    const FlecsWorldTransform3 *lane_xforms = ecs_field(it, FlecsWorldTransform3, 3);
    const TrafficCorner *corner = ecs_field(it, TrafficCorner, 4);
    bool corner_self = corner != NULL && ecs_field_is_self(it, 4);

    for (int row = 0; row < it->count; row++) {
        const TrafficLane *lane = &lanes[row];
        const TrafficLaneCars *cars = &lane_cars[row];
        const TrafficLaneCarEntities *ents = &lane_ents[row];
        const FlecsWorldTransform3 *lane_xform = &lane_xforms[row];
        const TrafficCorner *c = corner ? (corner_self ? &corner[row] : corner) : NULL;

        for (int i = 0; i < cars->count; i++) {
            ecs_entity_t e = ents->cars[i];
            if (!e || !ecs_is_alive(world, e)) continue;
            const TrafficCar *car = &cars->cars[i];

            vec3 local_pos = {0.0f, 0.5f, 0.0f};
            float yaw = 0;

            if (!c) {
                local_pos[0] = car->position - lane->length / 2.0f;
            } else {
                float pos = car->position;
                if (c->invert_direction) {
                    pos = lane->length - pos;
                }
                float t = (pos / lane->length) * ((float)GLM_PI / 2.0f);
                local_pos[0] = sinf(t) * c->radius - c->radius;
                local_pos[2] = cosf(t) * c->radius - c->radius;
                yaw = t;
                if (c->invert_direction) {
                    yaw -= (float)GLM_PI;
                }
            }

            vec3 world_pos;
            glm_mat4_mulv3((vec4*)lane_xform->m, local_pos, 1.0f, world_pos);

            float lane_yaw = atan2f(lane_xform->m[2][0], lane_xform->m[2][2]);

            ecs_set(world, e, FlecsPosition3,
                {world_pos[0], world_pos[1], world_pos[2]});
            ecs_set(world, e, FlecsRotation3,
                {0, lane_yaw + (float)GLM_PI / 2.0f + yaw, 0});
        }
    }
}

static void trafficCars_moveCarsToNextLane(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);
    TrafficLaneCarEntities *lane_ents = ecs_field(it, TrafficLaneCarEntities, 2);

    for (int row = 0; row < it->count; row++) {
        TrafficLane *lane = &lanes[row];
        TrafficLaneCars *cars = &lane_cars[row];
        TrafficLaneCarEntities *ents = &lane_ents[row];

        int i;
        for (i = 0; i < cars->count; i++) {
            TrafficCar *car = &cars->cars[i];
            if (car->position < lane->length) break;

            if (car->eol_state == TrafficEolWaitForIntersection ||
                car->eol_state == TrafficEolReserveIntersection ||
                car->eol_state == TrafficEolWaitForProtectedIntersection)
            {
                car->position = lane->length;
                car->speed = 0;
                car->target_speed = 0;
                car->state = TrafficCarStateStopped;
                break;
            }
        }

        if (!i) continue;
        if (!lane->next) {
            ecs_err("no lane to move to");
            continue;
        }

        for (int j = 0; j < i; j++) {
            TrafficCar *car = &cars->cars[j];
            ecs_entity_t lane_next = lane->next;
            uint8_t eol = TrafficEolDefault;

            if (car->eol_state == TrafficEolMoveOnIntersection) {
                lane_next = car->next_lane;
                eol = TrafficEolOnIntersection;
            } else if (car->eol_state == TrafficEolMoveOnProtectedIntersection) {
                lane_next = car->next_lane;
                eol = TrafficEolOnProtectedIntersection;
            } else if (car->eol_state == TrafficEolOnIntersection) {
                ecs_entity_t intersection = trafficCars_intersectionFromLane(
                    world, it->entities[row]);
                TrafficIntersectionRoads *ir = intersection ?
                    ecs_get_mut(world, intersection, TrafficIntersectionRoads) : NULL;
                if (ir) trafficCars_intersectionRelease(ir, car->reservation);
            }

            trafficCars_addCarToLane(world,
                lane_next,
                ents->cars[j],
                car->position - lane->length,
                car->speed,
                car->target_speed,
                car->state,
                eol,
                car->reservation);
        }

        memmove(cars->cars, cars->cars + i,
            (TRAFFIC_MAX_CARS_PER_LANE - i) * sizeof(TrafficCar));
        memmove(ents->cars, ents->cars + i,
            (TRAFFIC_MAX_CARS_PER_LANE - i) * sizeof(ecs_entity_t));
        cars->count -= i;
        if (cars->count < 0) cars->count = 0;
    }
}

void TrafficCarsImport(ecs_world_t *world) {
    ECS_MODULE(world, TrafficCars);

    ecs_set_name_prefix(world, "Traffic");

    ECS_COMPONENT_DEFINE(world, TrafficCar);
    ECS_COMPONENT_DEFINE(world, TrafficLaneCars);
    ECS_COMPONENT_DEFINE(world, TrafficLaneCarEntities);
    ECS_COMPONENT_DEFINE(world, TrafficLane);
    ECS_COMPONENT_DEFINE(world, TrafficCorner);
    ECS_COMPONENT_DEFINE(world, TrafficRoadConnect);
    ECS_COMPONENT_DEFINE(world, TrafficRoad);
    ECS_COMPONENT_DEFINE(world, TrafficRoadLanes);
    ECS_COMPONENT_DEFINE(world, TrafficIntersection);
    ECS_COMPONENT_DEFINE(world, TrafficIntersectionRoads);
    ECS_COMPONENT_DEFINE(world, TrafficIntersectionMovement);

    ecs_struct(world, {
        .entity = ecs_id(TrafficCar),
        .members = {
            { .name = "position", .type = ecs_id(ecs_f32_t) },
            { .name = "length", .type = ecs_id(ecs_f32_t) },
            { .name = "mass", .type = ecs_id(ecs_f32_t) },
            { .name = "speed", .type = ecs_id(ecs_f32_t) },
            { .name = "target_speed", .type = ecs_id(ecs_f32_t) },
            { .name = "state", .type = ecs_id(ecs_u8_t) },
            { .name = "eol_state", .type = ecs_id(ecs_u8_t) },
            { .name = "reservation", .type = ecs_id(ecs_u8_t) },
            { .name = "wait_count", .type = ecs_id(ecs_u8_t) },
            { .name = "next_lane", .type = ecs_id(ecs_entity_t) }
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(TrafficLane),
        .members = {
            { .name = "length", .type = ecs_id(ecs_f32_t) },
            { .name = "width", .type = ecs_id(ecs_f32_t) },
            { .name = "max_speed", .type = ecs_id(ecs_f32_t) },
            { .name = "next", .type = ecs_id(ecs_entity_t) },
            { .name = "road", .type = ecs_id(ecs_entity_t) }
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(TrafficCorner),
        .members = {
            { .name = "radius", .type = ecs_id(ecs_f32_t) },
            { .name = "invert_direction", .type = ecs_id(ecs_bool_t) }
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(TrafficRoadConnect),
        .members = {
            { .name = "road", .type = ecs_id(ecs_entity_t) },
            { .name = "edge", .type = ecs_id(ecs_i8_t) }
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(TrafficRoad),
        .members = {
            { .name = "length", .type = ecs_id(ecs_f32_t) },
            { .name = "lane_width", .type = ecs_id(ecs_f32_t) },
            { .name = "max_speed", .type = ecs_id(ecs_f32_t) },
            { .name = "corner", .type = ecs_id(ecs_bool_t) },
            { .name = "invert_corner", .type = ecs_id(ecs_bool_t) },
            { .name = "next", .type = ecs_id(TrafficRoadConnect) },
            { .name = "intersection", .type = ecs_id(ecs_entity_t) }
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(TrafficIntersection),
        .members = {
            { .name = "roads", .type = ecs_id(TrafficRoadConnect), .count = 4 },
            { .name = "lane_width", .type = ecs_id(ecs_f32_t) },
            { .name = "max_speed", .type = ecs_id(ecs_f32_t) }
        }
    });

    ecs_entity_t prev_scope = ecs_set_scope(world, 0);
    TrafficCarRoot = ecs_entity(world, { .name = "cars" });
    TrafficRoadRoot = ecs_entity(world, { .name = "roads" });
    ecs_set_scope(world, prev_scope);

    ecs_add_pair(world, ecs_id(TrafficLane), EcsWith, ecs_id(TrafficLaneCars));
    ecs_add_pair(world, ecs_id(TrafficLane), EcsWith, ecs_id(TrafficLaneCarEntities));
    ecs_add_pair(world, ecs_id(TrafficRoad), EcsWith, ecs_id(TrafficRoadLanes));
    ecs_add_pair(world, ecs_id(TrafficIntersection), EcsWith,
        ecs_id(TrafficIntersectionRoads));

    ecs_observer(world, {
        .query.terms = {
            {.id = ecs_id(TrafficCorner), .src.id = EcsSelf}
        },
        .events = { EcsOnSet },
        .callback = trafficCars_onSetCorner
    });

    ECS_SYSTEM(world, trafficCars_incrementLaneTick, EcsPreUpdate, 0);

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneProgressCars",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            {.id = ecs_id(TrafficLaneCars), .inout = EcsInOut}
        },
        .callback = trafficCars_progressCars
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneSetTargetSpeed",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            {.id = ecs_id(TrafficLane), .inout = EcsIn},
            {.id = ecs_id(TrafficLaneCars), .inout = EcsInOut}
        },
        .callback = trafficCars_laneSetTargetSpeed
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneInitiateEol",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            {.id = ecs_id(TrafficLane), .inout = EcsIn},
            {.id = ecs_id(TrafficLaneCars), .inout = EcsInOut}
        },
        .callback = trafficCars_laneInitiateEol
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneSetEolState",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            {.id = ecs_id(TrafficLane), .inout = EcsIn},
            {.id = ecs_id(TrafficLaneCars), .inout = EcsInOut}
        },
        .callback = trafficCars_laneSetEolState
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneSetDrivingState",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            {.id = ecs_id(TrafficLaneCars), .inout = EcsInOut}
        },
        .callback = trafficCars_laneSetDrivingState
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneAccelerate",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            {.id = ecs_id(TrafficLaneCars), .inout = EcsInOut}
        },
        .callback = trafficCars_laneAccelerate
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneUpdateCarEntities",
            .add = ecs_ids(ecs_dependson(EcsPostUpdate))
        }),
        .query.terms = {
            {.id = ecs_id(TrafficLane), .inout = EcsIn},
            {.id = ecs_id(TrafficLaneCars), .inout = EcsIn},
            {.id = ecs_id(TrafficLaneCarEntities), .inout = EcsIn},
            {.id = ecs_id(FlecsWorldTransform3), .inout = EcsIn},
            {.id = ecs_id(TrafficCorner), .src.id = EcsSelf, .oper = EcsOptional, .inout = EcsIn}
        },
        .callback = trafficCars_updateCarEntities
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneMoveCarsToNextLane",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            {.id = ecs_id(TrafficLane), .inout = EcsInOut},
            {.id = ecs_id(TrafficLaneCars), .inout = EcsInOut},
            {.id = ecs_id(TrafficLaneCarEntities), .inout = EcsInOut}
        },
        .callback = trafficCars_moveCarsToNextLane
    });
}
