#include "traffic.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

ECS_COMPONENT_DECLARE(TrafficLight);
ECS_COMPONENT_DECLARE(TrafficCar);
ECS_COMPONENT_DECLARE(TrafficLaneCars);
ECS_COMPONENT_DECLARE(TrafficLaneCarEntities);
ECS_COMPONENT_DECLARE(TrafficLane);
ECS_COMPONENT_DECLARE(TrafficLaneTrafficLight);
ECS_COMPONENT_DECLARE(TrafficCorner);
ECS_COMPONENT_DECLARE(TrafficRoadConnect);
ECS_COMPONENT_DECLARE(TrafficRoad);
ECS_COMPONENT_DECLARE(TrafficRoadLanes);
ECS_COMPONENT_DECLARE(TrafficIntersection);
ECS_COMPONENT_DECLARE(TrafficIntersectionRoads);
ECS_COMPONENT_DECLARE(TrafficIntersectionMovement);

ECS_DECLARE(TrafficCarRoot);
ECS_DECLARE(TrafficRoadRoot);

uint8_t TrafficLightGreenTicks = 32;
uint8_t TrafficLightOrangeTicks = 8;
float TrafficAccelerationForce = 1.5f;
float TrafficBreakForce = 5.0f;
float TrafficHardBreakForce = 12.0f;
uint8_t TrafficMaxWaitCount = 60;
float TrafficPlaceholderCarMass = 10.0f;
float TrafficPlaceholderCarLength = 3.0f;

static int LaneTick = 0;

static ecs_entity_t SetLaneTransformSys = 0;
static ecs_entity_t ConnectRoadsSys = 0;
static ecs_entity_t ConnectIntersectionSys = 0;

static float trafficCars_minDistanceForSpeed(float s1) {
    if (s1 < 0.1f) {
        s1 = 0.1f;
    }
    return s1 * 50.0f;
}

static ecs_entity_t trafficCars_findNextLane(const TrafficIntersectionMovement *im) {
    int8_t turn = (int8_t)(rand() % 3);

    if (!im->lanes[turn]) {
        int i;
        for (i = 1; i < 3; i ++) {
            if (im->lanes[(turn + i) % 3]) {
                break;
            }
        }
        if (i == 3) {
            ecs_err("intersection without lanes :(");
        } else {
            turn = (int8_t)((turn + i) % 3);
        }
    }

    return im->lanes[turn];
}

static float trafficCars_spaceInLane(
    ecs_world_t *world,
    ecs_entity_t lane)
{
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
    const TrafficLane *l = ecs_get(world, next_lane, TrafficLane);
    if (!l || !l->next) {
        return 0;
    }
    return trafficCars_spaceInLane(world, l->next);
}

static bool trafficCars_carFitsInDestinationLane(
    ecs_world_t *world,
    ecs_entity_t next_lane,
    TrafficCar *car)
{
    float space = trafficCars_spaceInDestinationLane(world, next_lane);

    const TrafficLaneCars *cars = ecs_get(world, next_lane, TrafficLaneCars);
    if (cars) {
        for (int i = 0; i < cars->count; i ++) {
            space -= cars->cars[i].length;
        }
        space -= cars->count * 2.0f;
    }

    return space > car->length;
}

static void trafficCars_waitForLane(
    TrafficCar *car,
    const TrafficIntersectionMovement *im)
{
    car->wait_count ++;

    if (car->wait_count > TrafficMaxWaitCount) {
        car->next_lane = trafficCars_findNextLane(im);
        car->wait_count = 0;
    }
}

void trafficCars_addCarToLane(
    ecs_world_t *world,
    ecs_entity_t in_lane,
    ecs_entity_t out_lane,
    ecs_entity_t car_entity,
    float position,
    float speed,
    float target_speed,
    TrafficCarState state,
    TrafficCarEndOfLaneState eol_state,
    uint8_t reservation)
{
    (void)in_lane;

    if (!out_lane) {
        ecs_err("no lane to move to for car %u!", (uint32_t)car_entity);
        return;
    }

    TrafficLaneCars *cars = ecs_get_mut(world, out_lane, TrafficLaneCars);
    if (!cars) {
        ecs_err("lane is missing LaneCars component while moving car %u!",
            (uint32_t)car_entity);
        return;
    }

    if (cars->count >= TRAFFIC_MAX_CARS_PER_LANE) {
        ecs_err("lane already has max number of cars, can't move car %u",
            (uint32_t)car_entity);
        return;
    }

    if (cars->count) {
        TrafficCar *last_car = &cars->cars[cars->count - 1];
        if ((last_car->position - last_car->length) < position) {
            ecs_err("adding car %u to lane would cause a crash!",
                (uint32_t)car_entity);
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
    car->mass = TrafficPlaceholderCarMass;
    car->length = TrafficPlaceholderCarLength;
    car->reservation = reservation;

    TrafficLaneCarEntities *car_entities = ecs_get_mut(
        world, out_lane, TrafficLaneCarEntities);
    car_entities->cars[cars->count] = car_entity;

    cars->count ++;
    assert(cars->count < TRAFFIC_MAX_CARS_PER_LANE);
}

ecs_entity_t trafficCars_roadFromLane(
    ecs_world_t *world,
    ecs_entity_t lane)
{
    const TrafficLane *l = ecs_get(world, lane, TrafficLane);
    if (!l) return 0;
    return l->road;
}

ecs_entity_t trafficCars_intersectionFromLane(
    ecs_world_t *world,
    ecs_entity_t lane)
{
    ecs_entity_t road = trafficCars_roadFromLane(world, lane);
    if (!road) return 0;
    return ecs_get_parent(world, road);
}

/* Observer: when Corner is set, derive Lane.length from radius. */
static void trafficCars_onSetCorner(ecs_iter_t *it) {
    TrafficCorner *c = ecs_field(it, TrafficCorner, 0);
    for (int i = 0; i < it->count; i ++) {
        TrafficLane *l = ecs_ensure(it->world, it->entities[i], TrafficLane);
        l->length = c[i].radius * (float)GLM_PI * 0.5f;
    }
}

/* PostUpdate one-shot system: compute manual transform for lanes from their
 * road's transform, and tag them so the transform system skips them. */
static void trafficCars_setLaneTransform(ecs_iter_t *it) {
    ecs_world_t *world = it->world;

    while (ecs_iter_next(it)) {
        const TrafficRoad *roads = ecs_field(it, TrafficRoad, 0);
        const TrafficRoadLanes *rls = ecs_field(it, TrafficRoadLanes, 1);
        FlecsWorldTransform3 *m_roads = ecs_field(it, FlecsWorldTransform3, 2);

        for (int row = 0; row < it->count; row ++) {
            const TrafficRoadLanes *rl = &rls[row];
            FlecsWorldTransform3 *m_road = &m_roads[row];
            (void)roads;

            ecs_entity_t left = rl->lanes[1];
            ecs_entity_t right = rl->lanes[0];

            /* Mark lanes so they don't get transformed automatically by the
             * transform system. */
            ecs_add(world, left, FlecsManualTransform);
            ecs_add(world, right, FlecsManualTransform);

            FlecsWorldTransform3 *l_lt = ecs_ensure(
                world, left, FlecsWorldTransform3);
            FlecsWorldTransform3 *r_lt = ecs_ensure(
                world, right, FlecsWorldTransform3);

            const FlecsPosition3 *l_p = ecs_get(world, left, FlecsPosition3);
            const FlecsPosition3 *r_p = ecs_get(world, right, FlecsPosition3);

            const FlecsRotation3 *l_r = ecs_get(world, left, FlecsRotation3);
            const FlecsRotation3 *r_r = ecs_get(world, right, FlecsRotation3);

            if (l_p) {
                glm_translate_to(m_road->m, *(vec3*)l_p, l_lt->m);
            }
            if (r_p) {
                glm_translate_to(m_road->m, *(vec3*)r_p, r_lt->m);
            }

            if (l_r) {
                glm_rotate(l_lt->m, l_r->x, (vec3){1.0f, 0.0f, 0.0f});
                glm_rotate(l_lt->m, l_r->y, (vec3){0.0f, 1.0f, 0.0f});
                glm_rotate(l_lt->m, l_r->z, (vec3){0.0f, 0.0f, 1.0f});
            }

            if (r_r) {
                glm_rotate(r_lt->m, r_r->x, (vec3){1.0f, 0.0f, 0.0f});
                glm_rotate(r_lt->m, r_r->y, (vec3){0.0f, 1.0f, 0.0f});
                glm_rotate(r_lt->m, r_r->z, (vec3){0.0f, 0.0f, 1.0f});
            }
        }
    }

    ecs_enable(world, SetLaneTransformSys, false);
}

/* Connect two roads together. */
static void trafficCars_connectRoads(ecs_iter_t *it) {
    ecs_world_t *world = it->world;

    while (ecs_iter_next(it)) {
        const TrafficRoad *roads = ecs_field(it, TrafficRoad, 0);
        const TrafficRoadLanes *rls = ecs_field(it, TrafficRoadLanes, 1);

        for (int row = 0; row < it->count; row ++) {
            const TrafficRoad *r = &roads[row];
            const TrafficRoadLanes *rl = &rls[row];

            ecs_entity_t next = r->next.road;
            if (next) {
                int32_t edge = r->next.edge;
                const TrafficRoadLanes *rl_next = ecs_get(
                    world, next, TrafficRoadLanes);
                if (rl_next) {
                    TrafficLane *left = ecs_get_mut(
                        world, rl_next->lanes[edge], TrafficLane);
                    if (left) {
                        left->next = rl->lanes[edge];
                    }

                    TrafficLane *right = ecs_get_mut(
                        world, rl->lanes[1 - edge], TrafficLane);
                    if (right) {
                        right->next = rl_next->lanes[1 - edge];
                    }
                }
            }
        }
    }

    ecs_enable(world, ConnectRoadsSys, false);
}

/* Observer: create lanes for a road. */
static void trafficCars_createRoad(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficRoad *roads = ecs_field(it, TrafficRoad, 0);
    TrafficRoadLanes *rls = ecs_field(it, TrafficRoadLanes, 1);

    for (int row = 0; row < it->count; row ++) {
        ecs_entity_t e = it->entities[row];
        const TrafficRoad *r = &roads[row];
        TrafficRoadLanes *rl = &rls[row];

        if (rl->lanes[0]) ecs_delete(world, rl->lanes[0]);
        if (rl->lanes[1]) ecs_delete(world, rl->lanes[1]);

        ecs_entity_t left = ecs_new_w_pair(world, EcsChildOf, TrafficRoadRoot);
        ecs_entity_t right = ecs_new_w_pair(world, EcsChildOf, TrafficRoadRoot);

        if (!r->corner) {
            ecs_set(world, left, TrafficLane,
                {r->length, r->lane_width, r->max_speed, 0, e});
            ecs_set(world, left, FlecsPosition3, {0, 0, (r->lane_width / 2)});
            ecs_set(world, left, FlecsRotation3, {0, (float)GLM_PI, 0});

            ecs_set(world, right, TrafficLane,
                {r->length, r->lane_width, r->max_speed, 0, e});
            ecs_set(world, right, FlecsPosition3, {0, 0, -(r->lane_width / 2)});
        } else {
            float left_radius = r->lane_width * 1.5f;
            float left_speed = left_radius / 50.0f;
            if (left_speed > r->max_speed) left_speed = r->max_speed;

            ecs_set(world, left, TrafficLane,
                {r->length, r->lane_width, left_speed, 0, e});
            ecs_set(world, left, FlecsPosition3,
                {r->lane_width / 2, 0, r->lane_width / 2});
            ecs_set(world, left, TrafficCorner, { left_radius, true });

            float right_radius = r->lane_width / 2;
            float right_speed = right_radius / 30.0f;
            if (right_speed > r->max_speed) right_speed = r->max_speed;

            ecs_set(world, right, TrafficLane,
                {r->length, r->lane_width, right_speed, 0, e});
            ecs_set(world, right, FlecsPosition3,
                {-r->lane_width / 2, 0, -r->lane_width / 2});
            ecs_set(world, right, TrafficCorner, { right_radius, false });
        }

        if (r->invert_corner) {
            rl->lanes[0] = right;
            rl->lanes[1] = left;
        } else {
            rl->lanes[0] = left;
            rl->lanes[1] = right;
        }

        ecs_enable(world, ConnectRoadsSys, true);
        ecs_enable(world, SetLaneTransformSys, true);
    }
}

/* Helpers for connectIntersection */
static ecs_entity_t trafficCars_iRoad(
    const TrafficIntersection *i, TrafficDirection d)
{
    return i->roads[d].road;
}

static ecs_entity_t trafficCars_iRoadLane(
    ecs_world_t *world,
    const TrafficIntersection *i,
    TrafficDirection d,
    bool flip)
{
    ecs_entity_t road = i->roads[d].road;
    if (!road) return 0;
    int8_t lane = i->roads[d].edge;
    if (flip) lane = 1 - lane;
    const TrafficRoadLanes *rl = ecs_get(world, road, TrafficRoadLanes);
    if (!rl) return 0;
    return rl->lanes[lane];
}

static ecs_entity_t trafficCars_intersectionLane(
    ecs_world_t *world,
    const TrafficIntersectionRoads *ir,
    TrafficConnection c,
    int8_t lane)
{
    if (!ir->roads[c]) return 0;
    const TrafficRoadLanes *rl = ecs_get(
        world, ir->roads[c], TrafficRoadLanes);
    if (!rl) return 0;
    return rl->lanes[lane];
}

static ecs_entity_t trafficCars_intersectionMovement(
    ecs_world_t *world,
    ecs_entity_t e,
    TrafficIntersectionRoads *ir,
    TrafficDirection d,
    ecs_entity_t l0,
    ecs_entity_t l1,
    ecs_entity_t l2,
    bool *intersect)
{
    ecs_entity_t lanes[3] = {l0, l1, l2};
    ecs_entity_t lane = 0;
    int32_t count = 0;

    for (int i = 0; i < 3; i ++) {
        if (lanes[i]) {
            lane = lanes[i];
            count ++;
        }
    }

    if (count == 1) {
        *intersect = false;
        return lane;
    }

    ecs_entity_t mv = ecs_new_w_pair(world, EcsChildOf, TrafficRoadRoot);
    ecs_set(world, mv, TrafficIntersectionMovement,
        { e, { lanes[0], lanes[1], lanes[2] } });
    ir->movements[d] = mv;

    *intersect = true;
    return mv;
}

static void trafficCars_setLaneNext(
    ecs_world_t *world,
    ecs_entity_t lane,
    ecs_entity_t next)
{
    if (!lane) return;
    TrafficLane *l = ecs_get_mut(world, lane, TrafficLane);
    if (l) {
        l->next = next;
    }
}

/* One-shot system: connect lanes across the intersection. */
static void trafficCars_connectIntersection(ecs_iter_t *it) {
    ecs_world_t *world = it->world;

    while (ecs_iter_next(it)) {
        const TrafficIntersection *is = ecs_field(it, TrafficIntersection, 0);
        TrafficIntersectionRoads *irs = ecs_field(it, TrafficIntersectionRoads, 1);
        const FlecsPosition3 *ps = ecs_field(it, FlecsPosition3, 2);

        for (int row = 0; row < it->count; row ++) {
            ecs_entity_t e = it->entities[row];
            const TrafficIntersection *i = &is[row];
            TrafficIntersectionRoads *ir = &irs[row];
            const FlecsPosition3 *p = &ps[row];

            float light_y = 2.5f, light_offset = 1.5f;

            /* Top */
            if (trafficCars_iRoad(i, TrafficTop)) {
                bool intersect;
                ecs_entity_t mv = trafficCars_intersectionMovement(world, e, ir, TrafficTop,
                    trafficCars_intersectionLane(world, ir, TrafficTopToRight, 0),
                    trafficCars_intersectionLane(world, ir, TrafficTopToBottom, 0),
                    trafficCars_intersectionLane(world, ir, TrafficTopToLeft, 0),
                    &intersect);
                ecs_entity_t in_lane = trafficCars_iRoadLane(world, i, TrafficTop, true);
                trafficCars_setLaneNext(world, in_lane, mv);

                if (intersect && in_lane) {
                    TrafficLaneTrafficLight *ltl = ecs_ensure(
                        world, in_lane, TrafficLaneTrafficLight);
                    ecs_entity_t light = ecs_new(world);
                    ecs_set(world, light, TrafficLight, {2});
                    ecs_set(world, light, FlecsPosition3, {
                        p->x - i->lane_width - light_offset,
                        p->y + light_y,
                        p->z + i->lane_width + light_offset
                    });
                    ltl->light = light;
                }
            }

            /* Bottom */
            if (trafficCars_iRoad(i, TrafficBottom)) {
                bool intersect;
                ecs_entity_t mv = trafficCars_intersectionMovement(world, e, ir, TrafficBottom,
                    trafficCars_intersectionLane(world, ir, TrafficBottomToLeft, 0),
                    trafficCars_intersectionLane(world, ir, TrafficTopToBottom, 1),
                    trafficCars_intersectionLane(world, ir, TrafficBottomToRight, 0),
                    &intersect);
                ecs_entity_t in_lane = trafficCars_iRoadLane(world, i, TrafficBottom, true);
                trafficCars_setLaneNext(world, in_lane, mv);

                if (intersect && in_lane) {
                    TrafficLaneTrafficLight *ltl = ecs_ensure(
                        world, in_lane, TrafficLaneTrafficLight);
                    ecs_entity_t light = ecs_new(world);
                    ecs_set(world, light, TrafficLight, {2});
                    ecs_set(world, light, FlecsPosition3, {
                        p->x + i->lane_width + light_offset,
                        p->y + light_y,
                        p->z - i->lane_width - light_offset
                    });
                    ecs_set(world, light, FlecsRotation3, {0, (float)GLM_PI, 0});
                    ltl->light = light;
                }
            }

            /* Left */
            if (trafficCars_iRoad(i, TrafficLeft)) {
                bool intersect;
                ecs_entity_t mv = trafficCars_intersectionMovement(world, e, ir, TrafficLeft,
                    trafficCars_intersectionLane(world, ir, TrafficTopToLeft, 1),
                    trafficCars_intersectionLane(world, ir, TrafficLeftToRight, 0),
                    trafficCars_intersectionLane(world, ir, TrafficBottomToLeft, 1),
                    &intersect);
                ecs_entity_t in_lane = trafficCars_iRoadLane(world, i, TrafficLeft, true);
                trafficCars_setLaneNext(world, in_lane, mv);

                if (intersect && in_lane) {
                    TrafficLaneTrafficLight *ltl = ecs_ensure(
                        world, in_lane, TrafficLaneTrafficLight);
                    ecs_entity_t light = ecs_new(world);
                    ecs_set(world, light, TrafficLight, {2});
                    ecs_set(world, light, FlecsPosition3, {
                        p->x - i->lane_width - light_offset,
                        p->y + light_y,
                        p->z - i->lane_width - light_offset
                    });
                    ecs_set(world, light, FlecsRotation3, {0, (float)(GLM_PI * 1.5), 0});
                    ltl->light = light;
                }
            }

            /* Right */
            if (trafficCars_iRoad(i, TrafficRight)) {
                bool intersect;
                ecs_entity_t mv = trafficCars_intersectionMovement(world, e, ir, TrafficRight,
                    trafficCars_intersectionLane(world, ir, TrafficBottomToRight, 1),
                    trafficCars_intersectionLane(world, ir, TrafficLeftToRight, 1),
                    trafficCars_intersectionLane(world, ir, TrafficTopToRight, 1),
                    &intersect);
                ecs_entity_t in_lane = trafficCars_iRoadLane(world, i, TrafficRight, true);
                trafficCars_setLaneNext(world, in_lane, mv);

                if (intersect && in_lane) {
                    TrafficLaneTrafficLight *ltl = ecs_ensure(
                        world, in_lane, TrafficLaneTrafficLight);
                    ecs_entity_t light = ecs_new(world);
                    ecs_set(world, light, TrafficLight, {2});
                    ecs_set(world, light, FlecsPosition3, {
                        p->x + i->lane_width + light_offset,
                        p->y + light_y,
                        p->z + i->lane_width + light_offset
                    });
                    ecs_set(world, light, FlecsRotation3, {0, (float)(GLM_PI * 0.5), 0});
                    ltl->light = light;
                }
            }

            /* Connect movements to outgoing lanes */
            if (trafficCars_iRoad(i, TrafficTop) && trafficCars_iRoad(i, TrafficBottom)) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficTopToBottom, 0),
                    trafficCars_iRoadLane(world, i, TrafficBottom, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficTopToBottom, 1),
                    trafficCars_iRoadLane(world, i, TrafficTop, false));
            }

            if (trafficCars_iRoad(i, TrafficLeft) && trafficCars_iRoad(i, TrafficRight)) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficLeftToRight, 0),
                    trafficCars_iRoadLane(world, i, TrafficRight, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficLeftToRight, 1),
                    trafficCars_iRoadLane(world, i, TrafficLeft, false));
            }

            if (trafficCars_iRoad(i, TrafficTop) && trafficCars_iRoad(i, TrafficRight)) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficTopToRight, 0),
                    trafficCars_iRoadLane(world, i, TrafficRight, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficTopToRight, 1),
                    trafficCars_iRoadLane(world, i, TrafficTop, false));
            }

            if (trafficCars_iRoad(i, TrafficTop) && trafficCars_iRoad(i, TrafficLeft)) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficTopToLeft, 0),
                    trafficCars_iRoadLane(world, i, TrafficLeft, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficTopToLeft, 1),
                    trafficCars_iRoadLane(world, i, TrafficTop, false));
            }

            if (trafficCars_iRoad(i, TrafficBottom) && trafficCars_iRoad(i, TrafficRight)) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficBottomToRight, 0),
                    trafficCars_iRoadLane(world, i, TrafficRight, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficBottomToRight, 1),
                    trafficCars_iRoadLane(world, i, TrafficBottom, false));
            }

            if (trafficCars_iRoad(i, TrafficBottom) && trafficCars_iRoad(i, TrafficLeft)) {
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficBottomToLeft, 0),
                    trafficCars_iRoadLane(world, i, TrafficLeft, false));
                trafficCars_setLaneNext(world,
                    trafficCars_intersectionLane(world, ir, TrafficBottomToLeft, 1),
                    trafficCars_iRoadLane(world, i, TrafficBottom, false));
            }
        }
    }

    ecs_enable(world, ConnectIntersectionSys, false);
}

/* Observer: create roads for an intersection. */
static void trafficCars_createIntersection(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficIntersection *iss = ecs_field(it, TrafficIntersection, 0);
    TrafficIntersectionRoads *irs = ecs_field(it, TrafficIntersectionRoads, 1);

    for (int row = 0; row < it->count; row ++) {
        ecs_entity_t e = it->entities[row];
        const TrafficIntersection *i = &iss[row];
        TrafficIntersectionRoads *ir = &irs[row];

        ecs_delete_with(world, ecs_pair(EcsChildOf, e));

        /* top <-> down */
        if (i->roads[TrafficTop].road && i->roads[TrafficBottom].road) {
            ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
            ecs_set(world, road, TrafficRoad,
                { i->lane_width * 2, i->lane_width, i->max_speed, false });
            ecs_set(world, road, FlecsPosition3, {0, 0, 0});
            ecs_set(world, road, FlecsRotation3, {0, (float)(GLM_PI * 1.5), 0});
            ir->roads[TrafficTopToBottom] = road;
        }

        /* left <-> right */
        if (i->roads[TrafficLeft].road && i->roads[TrafficRight].road) {
            ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
            ecs_set(world, road, TrafficRoad,
                { i->lane_width * 2, i->lane_width, i->max_speed, false });
            ecs_set(world, road, FlecsPosition3, {0, 0, 0});
            ecs_set(world, road, FlecsRotation3, {0, (float)GLM_PI, 0});
            ir->roads[TrafficLeftToRight] = road;
        }

        /* top <-> left */
        if (i->roads[TrafficLeft].road && i->roads[TrafficTop].road) {
            ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
            ecs_set(world, road, TrafficRoad,
                { i->lane_width, i->lane_width, i->max_speed, true, true });
            ecs_set(world, road, FlecsPosition3, {0, 0, 0});
            ecs_set(world, road, FlecsRotation3, {0, (float)(GLM_PI / 2), 0});
            ir->roads[TrafficTopToLeft] = road;
        }

        /* top <-> right */
        if (i->roads[TrafficRight].road && i->roads[TrafficTop].road) {
            ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
            ecs_set(world, road, TrafficRoad,
                { i->lane_width, i->lane_width, i->max_speed, true });
            ecs_set(world, road, FlecsPosition3, {0, 0, 0});
            ecs_set(world, road, FlecsRotation3, {0, (float)GLM_PI, 0});
            ir->roads[TrafficTopToRight] = road;
        }

        /* bottom <-> right */
        if (i->roads[TrafficRight].road && i->roads[TrafficBottom].road) {
            ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
            ecs_set(world, road, TrafficRoad,
                { i->lane_width, i->lane_width, i->max_speed, true, true });
            ecs_set(world, road, FlecsPosition3, {0, 0, 0});
            ecs_set(world, road, FlecsRotation3, {0, (float)(GLM_PI * 1.5), 0});
            ir->roads[TrafficBottomToRight] = road;
        }

        /* bottom <-> left */
        if (i->roads[TrafficLeft].road && i->roads[TrafficBottom].road) {
            ecs_entity_t road = ecs_new_w_pair(world, EcsChildOf, e);
            ecs_set(world, road, TrafficRoad,
                { i->lane_width, i->lane_width, i->max_speed, true });
            ecs_set(world, road, FlecsPosition3, {0, 0, 0});
            ir->roads[TrafficBottomToLeft] = road;
        }

        ecs_enable(world, ConnectIntersectionSys, true);
    }
}

/* Increment LaneTick each frame. */
static void trafficCars_incrementLaneTick(ecs_iter_t *it) {
    (void)it;
    LaneTick ++;
    if (LaneTick == 8) {
        LaneTick = 0;
    }
}

/* Move cars along their lane based on their speed. */
static void trafficCars_laneProgressCars(ecs_iter_t *it) {
    TrafficLaneCars *cars = ecs_field(it, TrafficLaneCars, 0);
    for (int row = 0; row < it->count; row ++) {
        for (int j = 0; j < cars[row].count; j ++) {
            cars[row].cars[j].position += cars[row].cars[j].speed;
        }
    }
}

/* Per-car target speed update for a single car relative to the next car. */
static void trafficCars_setTargetSpeed(
    TrafficCar *car,
    float next_position,
    float next_speed,
    TrafficCarState next_state,
    float lane_max_speed)
{
    if (car->state == TrafficCarStateCrashed) {
        return;
    }

    float distance = next_position - car->position;
    float min_d = trafficCars_minDistanceForSpeed(car->speed);
    float target_speed = lane_max_speed;

    if (distance < 0) {
        ecs_err("crash happened! (distance = %.2f, position = %.2f, "
            "next_position = %.2f, speed = %.2f, target_speed = %.2f)",
            distance, car->position, next_position,
            car->speed, car->target_speed);
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

    if (car->eol_state == TrafficCarEolWaitForIntersection ||
        car->eol_state == TrafficCarEolReserveIntersection ||
        car->eol_state == TrafficCarEolWaitForProtectedIntersection)
    {
        target_speed = 0;
    }

    car->target_speed = target_speed;
}

static void trafficCars_laneCarSetTargetSpeed(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);

    for (int row = 0; row < it->count; row ++) {
        const TrafficLane *lane = &lanes[row];
        TrafficLaneCars *cars = &lane_cars[row];

        if (!cars->count) continue;

        if ((((uint64_t)it->entities[row]) & 7) != (uint64_t)LaneTick) continue;

        TrafficCar *car = &cars->cars[0];
        ecs_entity_t next_lane = lane->next;

        bool intersection = false;
        const TrafficLane *nl = next_lane ? ecs_get(world, next_lane, TrafficLane) : NULL;
        if (!nl && next_lane) {
            intersection = true;
            if (car->next_lane) {
                next_lane = car->next_lane;
                nl = ecs_get(world, next_lane, TrafficLane);
                assert(nl != NULL);
            }
        }

        float next_position_offset = 0;
        const TrafficCar *next_car = NULL;
        if (nl) {
            const TrafficLaneCars *nlc = ecs_get(world, next_lane, TrafficLaneCars);
            if (nlc && nlc->count) {
                next_car = &nlc->cars[nlc->count - 1];
            } else if (intersection) {
                const TrafficLaneCars *nnlc = nl->next ?
                    ecs_get(world, nl->next, TrafficLaneCars) : NULL;
                if (nnlc && nnlc->count) {
                    next_car = &nnlc->cars[nnlc->count - 1];
                    next_position_offset = nl->length;
                }
            }
        }

        if (next_car) {
            float next_position = next_car->position - next_car->length / 2.0f;
            next_position += next_position_offset;
            next_position += lane->length;
            trafficCars_setTargetSpeed(car, next_position,
                next_car->speed, next_car->state, lane->max_speed);
        } else {
            trafficCars_setTargetSpeed(car, 1000.0f * 1000.0f, 1000.0f * 1000.0f,
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

        if (car->target_speed == 0 &&
            car->eol_state == TrafficCarEolMoveOnIntersection)
        {
            ecs_err("car is stopping while moving on intersection");
        }

        for (int i = 1; i < cars->count; i ++) {
            TrafficCar *c = &cars->cars[i];
            TrafficCar *next = &cars->cars[i - 1];
            float next_position = next->position - next->length;
            trafficCars_setTargetSpeed(c, next_position,
                next->speed, next->state, lane->max_speed);
        }
    }
}

/* Reserve / release the intersection through the lane's traffic light. */
static void trafficCars_laneHandleTrafficLight(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    const TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);
    TrafficLaneTrafficLight *ltls = ecs_field(it, TrafficLaneTrafficLight, 2);

    for (int row = 0; row < it->count; row ++) {
        if ((((uint64_t)it->entities[row]) & 7) != (uint64_t)LaneTick) continue;

        const TrafficLane *lane = &lanes[row];
        const TrafficLaneCars *cars = &lane_cars[row];
        TrafficLaneTrafficLight *ltl = &ltls[row];

        if (!ltl->light) continue;

        ecs_entity_t next_lane = lane->next;
        const TrafficIntersectionMovement *im = ecs_get(
            world, next_lane, TrafficIntersectionMovement);
        if (!im) continue;
        TrafficIntersectionRoads *ir = ecs_get_mut(
            world, im->intersection, TrafficIntersectionRoads);
        if (!ir) continue;

        switch (ltl->state) {
        case TrafficLaneTrafficLightDefault:
            if (!cars->count) break;
            ltl->reservation = ir->next_reservation ++;
            ltl->state = TrafficLaneTrafficLightReserved;
            break;

        case TrafficLaneTrafficLightReserved:
            if (!cars->count) {
                if (ltl->reservation == ir->current_reservation) {
                    ltl->state = TrafficLaneTrafficLightDefault;
                    ir->current_reservation ++;
                }
                break;
            }
            if (ltl->reservation == ir->current_reservation) {
                ltl->state = TrafficLaneTrafficLightAcquired;
                ltl->timer = TrafficLightGreenTicks;
                ecs_set(world, ltl->light, TrafficLight, {0});
            }
            break;

        case TrafficLaneTrafficLightAcquired:
            ltl->timer --;
            if (!ltl->timer) {
                ltl->state = TrafficLaneTrafficLightReleasing;
                ltl->timer = TrafficLightOrangeTicks;
                ecs_set(world, ltl->light, TrafficLight, {1});
            }
            break;

        case TrafficLaneTrafficLightReleasing:
            ltl->timer --;
            if (!ltl->timer) {
                assert(ltl->reservation == ir->current_reservation);
                ir->current_reservation ++;
                ltl->state = TrafficLaneTrafficLightDefault;
                ecs_set(world, ltl->light, TrafficLight, {2});
            }
            break;
        }
    }
}

/* If the car is approaching the end of the lane and connects to something
 * special (intersection / traffic light), kick off end-of-lane behavior. */
static void trafficCars_laneInitiateEndOfLaneBehavior(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    TrafficLaneTrafficLight *ltls = ecs_field(it, TrafficLaneTrafficLight, 1);
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 2);

    for (int row = 0; row < it->count; row ++) {
        if ((((uint64_t)it->entities[row]) & 7) != (uint64_t)LaneTick) continue;

        const TrafficLane *lane = &lanes[row];
        TrafficLaneCars *cars = &lane_cars[row];
        TrafficLaneTrafficLight *ltl = ltls ? &ltls[row] : NULL;

        if (!cars->count) continue;

        ecs_entity_t next_lane = lane->next;
        TrafficCar *car = &cars->cars[0];

        if (car->eol_state != TrafficCarEolDefault) continue;

        float min_d = trafficCars_minDistanceForSpeed(car->speed);
        if (min_d < 1) min_d = 1;

        if ((lane->length - car->position) > min_d) continue;

        if (ltl) {
            const TrafficIntersectionMovement *im = ecs_get(
                world, next_lane, TrafficIntersectionMovement);
            if (im) {
                car->eol_state = TrafficCarEolWaitForProtectedIntersection;
                car->next_lane = trafficCars_findNextLane(im);
                car->target_speed = 0;
            }
        } else if (next_lane && !ecs_has(world, next_lane, TrafficLane)) {
            car->eol_state = TrafficCarEolReserveIntersection;
            car->target_speed = 0;
        }
    }
}

/* Resolve end-of-lane state transitions (reserve / acquire intersection). */
static void trafficCars_laneSetEndOfLaneState(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    TrafficLaneTrafficLight *ltls = ecs_field(it, TrafficLaneTrafficLight, 1);
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 2);

    for (int row = 0; row < it->count; row ++) {
        if ((((uint64_t)it->entities[row]) & 7) != (uint64_t)LaneTick) continue;

        const TrafficLane *lane = &lanes[row];
        TrafficLaneCars *cars = &lane_cars[row];
        TrafficLaneTrafficLight *ltl = ltls ? &ltls[row] : NULL;

        if (!cars->count) continue;

        TrafficCar *car = &cars->cars[0];

        switch (car->eol_state) {
        case TrafficCarEolDefault:
            car->wait_count = 0;
            /* fallthrough */
        case TrafficCarEolMoveOnIntersection:
        case TrafficCarEolMoveOnProtectedIntersection:
        case TrafficCarEolOnIntersection:
        case TrafficCarEolOnProtectedIntersection:
            continue;
        default:
            break;
        }

        ecs_entity_t next_lane = lane->next;
        const TrafficIntersectionMovement *im = ecs_get(
            world, next_lane, TrafficIntersectionMovement);
        if (!im) continue;
        TrafficIntersectionRoads *ir = ecs_get_mut(
            world, im->intersection, TrafficIntersectionRoads);
        if (!ir) continue;

        switch (car->eol_state) {
        case TrafficCarEolReserveIntersection: {
            assert(ltl == NULL);
            car->reservation = ir->next_reservation ++;
            car->next_lane = trafficCars_findNextLane(im);
            car->eol_state = TrafficCarEolWaitForIntersection;
            car->target_speed = 0;
            break;
        }

        case TrafficCarEolWaitForIntersection:
            if (ir->current_reservation == car->reservation) {
                ecs_entity_t dst_lane = car->next_lane;
                const TrafficLaneCars *dlc = ecs_get(world, dst_lane, TrafficLaneCars);
                assert(dlc && dlc->count == 0);
                (void)dlc;
                if (trafficCars_carFitsInDestinationLane(world, dst_lane, car)) {
                    car->eol_state = TrafficCarEolMoveOnIntersection;
                } else {
                    assert(car->reservation == ir->current_reservation);
                    ir->current_reservation ++;
                    car->eol_state = TrafficCarEolReserveIntersection;
                }
            } else {
                trafficCars_waitForLane(car, im);
            }
            break;

        case TrafficCarEolWaitForProtectedIntersection:
            assert(ltl != NULL);
            if (ltl->state == TrafficLaneTrafficLightAcquired) {
                ecs_entity_t dst_lane = car->next_lane;
                if (trafficCars_carFitsInDestinationLane(world, dst_lane, car)) {
                    car->eol_state = TrafficCarEolMoveOnProtectedIntersection;
                } else {
                    trafficCars_waitForLane(car, im);
                }
            }
            break;

        default:
            break;
        }
    }
}

static void trafficCars_setDrivingState(TrafficCar *car) {
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

static void trafficCars_laneCarSetDrivingState(ecs_iter_t *it) {
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);

    for (int row = 0; row < it->count; row ++) {
        if ((((uint64_t)it->entities[row]) & 7) != (uint64_t)LaneTick) continue;

        TrafficLaneCars *cars = &lane_cars[row];
        for (int i = 0; i < cars->count; i ++) {
            TrafficCar *car = &cars->cars[i];
            if (car->state == TrafficCarStateCrashed) continue;
            trafficCars_setDrivingState(car);
        }
    }
}

static void trafficCars_laneAccelerateCars(ecs_iter_t *it) {
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 0);
    float dt = it->delta_system_time;
    float a = TrafficAccelerationForce * dt;
    float b = TrafficBreakForce * dt;
    float hb = TrafficHardBreakForce * dt;

    for (int row = 0; row < it->count; row ++) {
        TrafficLaneCars *cars = &lane_cars[row];
        for (int i = 0; i < cars->count; i ++) {
            TrafficCar *car = &cars->cars[i];
            if (car->state == TrafficCarStateCrashed) continue;

            switch (car->state) {
            case TrafficCarStateAccelerating:
                assert(car->speed <= car->target_speed);
                car->speed += a / car->mass;
                if (car->speed > car->target_speed) {
                    car->speed = car->target_speed;
                    car->state = TrafficCarStateDriving;
                }
                break;
            case TrafficCarStateBreaking:
                assert(car->speed >= car->target_speed);
                car->speed -= b / car->mass;
                if (car->speed < car->target_speed) {
                    car->speed = car->target_speed;
                    car->state = TrafficCarStateDriving;
                }
                break;
            case TrafficCarStateBreakingHard:
                assert(car->speed >= car->target_speed);
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
                if (car->state != TrafficCarStateStopped) {
                    if (car->eol_state == TrafficCarEolOnIntersection) {
                        const TrafficLaneCarEntities *ents = ecs_get(
                            it->world, it->entities[row], TrafficLaneCarEntities);
                        ecs_err("[%u] car stopped on intersection",
                            (uint32_t)(ents ? ents->cars[i] : 0));
                    }
                }
                car->state = TrafficCarStateStopped;
                car->speed = 0;
            }
        }
    }
}

/* Update the car entity transforms based on lane geometry. */
static void trafficCars_laneUpdateCarEntities(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    const TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    const TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);
    const TrafficLaneCarEntities *lane_ents = ecs_field(it, TrafficLaneCarEntities, 2);
    FlecsWorldTransform3 *lane_xforms = ecs_field(it, FlecsWorldTransform3, 3);
    const TrafficCorner *corner = ecs_field(it, TrafficCorner, 4);

    bool is_dynamic = ecs_table_has_id(it->real_world, it->table, FlecsDynamicTransform);

    for (int row = 0; row < it->count; row ++) {
        const TrafficLane *lane = &lanes[row];
        const TrafficLaneCars *cars = &lane_cars[row];
        const TrafficLaneCarEntities *ents = &lane_ents[row];
        FlecsWorldTransform3 *transform = &lane_xforms[row];
        const TrafficCorner *c = corner ? &corner[row] : NULL;

        for (int i = 0; i < cars->count; i ++) {
            ecs_entity_t e = ents->cars[i];
            const TrafficCar *car = &cars->cars[i];

            vec4 p = {0.0f, 0.0f, 0.0f, 1.0f};
            float t = 0;

            if (!c) {
                p[0] = car->position - lane->length / 2.0f;
            } else {
                float pos = car->position;
                if (c->invert_direction) {
                    pos = lane->length - pos;
                }
                t = (pos / lane->length) * ((float)GLM_PI / 2.0f);
                p[0] = sinf(t) * c->radius - c->radius;
                p[2] = cosf(t) * c->radius - c->radius;
            }

            /* Compose lane-relative position into world space, set
             * Position3/Rotation3/Scale3 directly so the standard transform
             * pipeline can propagate to prefab children (Gltf meshes). */
            vec3 world_pos;
            glm_mat4_mulv3(transform->m, (vec3){p[0], p[1], p[2]}, 1.0f, world_pos);
            float lane_yaw = atan2f(transform->m[2][0], transform->m[2][2]);
            float yaw_offset = (c && c->invert_direction) ? (t - (float)GLM_PI) : t;

            // if (is_dynamic) {
                {
                    FlecsPosition3 *ptr = ecs_get_mut(world, e, FlecsPosition3);
                    ptr->x = world_pos[0];
                    ptr->y = world_pos[1];
                    ptr->z = world_pos[2];
                }
                {
                    FlecsRotation3 *ptr = ecs_get_mut(world, e, FlecsRotation3);
                    ptr->x = 0;
                    ptr->y = lane_yaw + yaw_offset + (float)(GLM_PI * 0.5f);
                    ptr->z = 0;
                }

            // } else {
            //     ecs_set(world, e, FlecsPosition3, {world_pos[0], world_pos[1], world_pos[2]});
            //     ecs_set(world, e, FlecsRotation3,
            //         {0, lane_yaw + yaw_offset + (float)(GLM_PI * 0.5f), 0});
            //     ecs_set(world, e, FlecsScale3, {1.0f, 1.0f, 1.0f});
            // }

#ifndef NDEBUG
            ecs_set_ptr(world, e, TrafficCar, car);
#endif
        }
    }
}

/* Move cars across lane boundaries when they reach the end of their lane. */
static void trafficCars_laneMoveCarsToNextLane(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    TrafficLane *lanes = ecs_field(it, TrafficLane, 0);
    TrafficLaneCars *lane_cars = ecs_field(it, TrafficLaneCars, 1);
    TrafficLaneCarEntities *lane_ents = ecs_field(it, TrafficLaneCarEntities, 2);
    TrafficLaneTrafficLight *ltls = ecs_field(it, TrafficLaneTrafficLight, 3);
    (void)ltls;

    for (int row = 0; row < it->count; row ++) {
        TrafficLane *lane = &lanes[row];
        TrafficLaneCars *cars = &lane_cars[row];
        TrafficLaneCarEntities *ents = &lane_ents[row];

        int i;
        for (i = 0; i < cars->count; i ++) {
            TrafficCar *car = &cars->cars[i];

            if (car->position < lane->length) {
                break;
            }

            if (car->eol_state == TrafficCarEolWaitForIntersection ||
                car->eol_state == TrafficCarEolReserveIntersection ||
                car->eol_state == TrafficCarEolWaitForProtectedIntersection)
            {
                car->position = lane->length;
                car->speed = 0;
                car->target_speed = 0;
                car->state = TrafficCarStateStopped;
                break;
            }
        }

        if (i) {
            if (lane->next) {
                for (int j = 0; j < i; j ++) {
                    TrafficCar *car = &cars->cars[j];
                    ecs_entity_t lane_next = lane->next;
                    TrafficCarEndOfLaneState eol = TrafficCarEolDefault;

                    if (car->eol_state == TrafficCarEolMoveOnIntersection) {
                        lane_next = car->next_lane;
                        eol = TrafficCarEolOnIntersection;
                    } else if (car->eol_state == TrafficCarEolMoveOnProtectedIntersection) {
                        lane_next = car->next_lane;
                        eol = TrafficCarEolOnProtectedIntersection;
                    } else if (car->eol_state == TrafficCarEolOnIntersection) {
                        ecs_entity_t intersection =
                            trafficCars_intersectionFromLane(
                                world, it->entities[row]);
                        TrafficIntersectionRoads *ir = ecs_get_mut(
                            world, intersection, TrafficIntersectionRoads);
                        if (ir) {
                            assert(car->reservation == ir->current_reservation);
                            ir->current_reservation ++;
                        }
                    } else if (car->eol_state == TrafficCarEolOnProtectedIntersection) {
                        /* Reservation is held by the lane for protected
                         * intersections; no release here. */
                    } else if (car->eol_state != TrafficCarEolDefault) {
                        ecs_err(
                            "car %u doesn't have the right state to move on intersection",
                            (uint32_t)ents->cars[j]);
                    }

                    trafficCars_addCarToLane(world,
                        it->entities[row],
                        lane_next,
                        ents->cars[j],
                        (car->position - lane->length),
                        car->speed,
                        car->target_speed,
                        car->state,
                        eol,
                        car->reservation);
                }
            } else {
                ecs_err("no lane to move to!");
            }

            memmove(cars->cars, cars->cars + i,
                (TRAFFIC_MAX_CARS_PER_LANE - i) * sizeof(TrafficCar));
            memmove(ents->cars, ents->cars + i,
                (TRAFFIC_MAX_CARS_PER_LANE - i) * sizeof(ecs_entity_t));

            cars->count -= (int8_t)i;
            assert(cars->count >= 0);
        }
    }
}

void TrafficCarsImport(ecs_world_t *world) {
    ECS_MODULE(world, TrafficCars);

    ecs_set_name_prefix(world, "Traffic");

    ECS_COMPONENT_DEFINE(world, TrafficLight);
    ECS_COMPONENT_DEFINE(world, TrafficCar);
    ECS_COMPONENT_DEFINE(world, TrafficLaneCars);
    ECS_COMPONENT_DEFINE(world, TrafficLaneCarEntities);
    ECS_COMPONENT_DEFINE(world, TrafficLane);
    ECS_COMPONENT_DEFINE(world, TrafficLaneTrafficLight);
    ECS_COMPONENT_DEFINE(world, TrafficCorner);
    ECS_COMPONENT_DEFINE(world, TrafficRoadConnect);
    ECS_COMPONENT_DEFINE(world, TrafficRoad);
    ECS_COMPONENT_DEFINE(world, TrafficRoadLanes);
    ECS_COMPONENT_DEFINE(world, TrafficIntersection);
    ECS_COMPONENT_DEFINE(world, TrafficIntersectionRoads);
    ECS_COMPONENT_DEFINE(world, TrafficIntersectionMovement);

    /* Keep TrafficLight named "TrafficLight" so script templates that bind
     * to traffic.cars.TrafficLight can find it. */
    ecs_set_name(world, ecs_id(TrafficLight), "TrafficLight");

    /* Reflection */
    ecs_struct(world, {
        .entity = ecs_id(TrafficLight),
        .members = {
            { .name = "state", .type = ecs_id(ecs_i8_t) }
        }
    });

    ecs_entity_t car_state_e = ecs_enum(world, {
        .entity = ecs_entity(world, { .name = "Car.State" }),
        .constants = {
            { .name = "Unknown" },
            { .name = "Accelerating" },
            { .name = "Driving" },
            { .name = "Breaking" },
            { .name = "BreakingHard" },
            { .name = "Stopped" },
            { .name = "Crashed" }
        }
    });

    ecs_entity_t car_eol_state_e = ecs_enum(world, {
        .entity = ecs_entity(world, { .name = "Car.EndOfLaneState" }),
        .constants = {
            { .name = "Default" },
            { .name = "ReserveIntersection" },
            { .name = "WaitForIntersection" },
            { .name = "MoveOnIntersection" },
            { .name = "OnIntersection" },
            { .name = "WaitForProtectedIntersection" },
            { .name = "MoveOnProtectedIntersection" },
            { .name = "OnProtectedIntersection" }
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(TrafficCar),
        .members = {
            { .name = "position", .type = ecs_id(ecs_f32_t) },
            { .name = "length", .type = ecs_id(ecs_f32_t) },
            { .name = "mass", .type = ecs_id(ecs_f32_t) },
            { .name = "speed", .type = ecs_id(ecs_f32_t) },
            { .name = "target_speed", .type = ecs_id(ecs_f32_t) },
            { .name = "state", .type = car_state_e },
            { .name = "eol_state", .type = car_eol_state_e },
            { .name = "reservation", .type = ecs_id(ecs_u8_t) },
            { .name = "wait_count", .type = ecs_id(ecs_u8_t) },
            { .name = "next_lane", .type = ecs_id(ecs_entity_t) }
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(TrafficLaneCars),
        .members = {
            { .name = "cars", .type = ecs_id(TrafficCar),
              .count = TRAFFIC_MAX_CARS_PER_LANE },
            { .name = "count", .type = ecs_id(ecs_i8_t) }
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(TrafficLaneCarEntities),
        .members = {
            { .name = "cars", .type = ecs_id(ecs_entity_t),
              .count = TRAFFIC_MAX_CARS_PER_LANE }
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
        .entity = ecs_id(TrafficRoadLanes),
        .members = {
            { .name = "lanes", .type = ecs_id(ecs_entity_t), .count = 2 }
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
        .entity = ecs_id(TrafficIntersectionRoads),
        .members = {
            { .name = "roads", .type = ecs_id(ecs_entity_t), .count = 6 },
            { .name = "movements", .type = ecs_id(ecs_entity_t), .count = 4 },
            { .name = "current_reservation", .type = ecs_id(ecs_u8_t) },
            { .name = "next_reservation", .type = ecs_id(ecs_u8_t) }
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

    ecs_struct(world, {
        .entity = ecs_id(TrafficIntersectionMovement),
        .members = {
            { .name = "intersection", .type = ecs_id(ecs_entity_t) },
            { .name = "lanes", .type = ecs_id(ecs_entity_t), .count = 3 }
        }
    });

    /* Scopes for storing road and car entities at world root. */
    ecs_entity_t prev_scope = ecs_set_scope(world, 0);
    TrafficCarRoot = ecs_entity(world, { .name = "cars" });
    TrafficRoadRoot = ecs_entity(world, { .name = "roads" });
    ecs_set_scope(world, prev_scope);

    /* With hooks (mirroring the C++ port).  Cars don't get
     * FlecsManualTransform: their Position3 is set every frame from the lane
     * transform so the standard OnSet observer can propagate the result to
     * any prefab-instantiated children (e.g. Gltf meshes). */
    ecs_add_pair(world, ecs_id(TrafficCar), EcsWith,
        ecs_id(FlecsPosition3));
    ecs_add_pair(world, ecs_id(TrafficCar), EcsWith,
        ecs_id(FlecsRotation3));
    ecs_add_pair(world, ecs_id(TrafficCar), EcsWith,
        ecs_id(FlecsScale3));

    ecs_add_pair(world, ecs_id(TrafficLane), EcsWith, ecs_id(TrafficLaneCars));
    ecs_add_pair(world, ecs_id(TrafficLane), EcsWith, ecs_id(TrafficLaneCarEntities));

    ecs_add_pair(world, ecs_id(TrafficRoad), EcsWith, ecs_id(TrafficRoadLanes));

    ecs_add_pair(world, ecs_id(TrafficIntersection), EcsWith,
        ecs_id(TrafficIntersectionRoads));

    /* Default zero-init for components with non-trivial layouts. */
    ecs_set_hooks(world, TrafficLaneCars, { .ctor = flecs_default_ctor });
    ecs_set_hooks(world, TrafficLaneCarEntities, { .ctor = flecs_default_ctor });
    ecs_set_hooks(world, TrafficLaneTrafficLight, { .ctor = flecs_default_ctor });
    ecs_set_hooks(world, TrafficIntersectionRoads, { .ctor = flecs_default_ctor });

    /* Observer: derive Lane.length from Corner.radius. */
    ecs_observer(world, {
        .query.terms = {
            { .id = ecs_id(TrafficCorner), .src.id = EcsSelf }
        },
        .events = { EcsOnSet },
        .callback = trafficCars_onSetCorner
    });

    /* Observer: create lanes for a road. */
    ecs_observer(world, {
        .entity = ecs_entity(world, { .name = "CreateRoad" }),
        .query.terms = {
            { .id = ecs_id(TrafficRoad), .inout = EcsIn },
            { .id = ecs_id(TrafficRoadLanes), .inout = EcsInOut }
        },
        .events = { EcsOnSet },
        .callback = trafficCars_createRoad
    });

    /* Observer: create connecting roads on intersection. */
    ecs_observer(world, {
        .entity = ecs_entity(world, { .name = "CreateIntersection" }),
        .query.terms = {
            { .id = ecs_id(TrafficIntersection), .inout = EcsIn },
            { .id = ecs_id(TrafficIntersectionRoads), .inout = EcsInOut }
        },
        .events = { EcsOnSet },
        .callback = trafficCars_createIntersection
    });

    /* IncrementLaneTick: every-frame system in OnUpdate. */
    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "IncrementLaneTick",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .callback = trafficCars_incrementLaneTick
    });

    /* PostUpdate one-shot: SetLaneTransform. */
    SetLaneTransformSys = ecs_system(world, {
        .entity = ecs_entity(world, { .name = "SetLaneTransform" }),
        .query.terms = {
            { .id = ecs_id(TrafficRoad), .inout = EcsIn },
            { .id = ecs_id(TrafficRoadLanes), .inout = EcsIn },
            { .id = ecs_id(FlecsWorldTransform3), .inout = EcsInOut }
        },
        .phase = EcsPostUpdate,
        .immediate = true,
        .run = trafficCars_setLaneTransform
    });

    /* OnUpdate one-shot: ConnectRoads. */
    ConnectRoadsSys = ecs_system(world, {
        .entity = ecs_entity(world, { .name = "ConnectRoads" }),
        .query.terms = {
            { .id = ecs_id(TrafficRoad), .inout = EcsIn },
            { .id = ecs_id(TrafficRoadLanes), .inout = EcsIn }
        },
        .phase = EcsOnUpdate,
        .immediate = true,
        .run = trafficCars_connectRoads
    });

    /* OnUpdate one-shot: ConnectIntersections. */
    ConnectIntersectionSys = ecs_system(world, {
        .entity = ecs_entity(world, { .name = "ConnectIntersections" }),
        .query.terms = {
            { .id = ecs_id(TrafficIntersection), .inout = EcsIn },
            { .id = ecs_id(TrafficIntersectionRoads), .inout = EcsInOut },
            { .id = ecs_id(FlecsPosition3), .inout = EcsIn }
        },
        .phase = EcsOnUpdate,
        .immediate = true,
        .run = trafficCars_connectIntersection
    });

    /* OnUpdate per-frame systems, in original execution order. */

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneProgressCars",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLaneCars), .inout = EcsInOut }
        },
        .callback = trafficCars_laneProgressCars
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneCarSetTargetSpeed",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLane), .inout = EcsIn },
            { .id = ecs_id(TrafficLaneCars), .inout = EcsInOut }
        },
        .callback = trafficCars_laneCarSetTargetSpeed
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneHandleTrafficLight",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLane), .inout = EcsIn },
            { .id = ecs_id(TrafficLaneCars), .inout = EcsIn },
            { .id = ecs_id(TrafficLaneTrafficLight), .inout = EcsInOut }
        },
        .callback = trafficCars_laneHandleTrafficLight
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneInitiateEndOfLaneBehavior",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLane), .inout = EcsIn },
            { .id = ecs_id(TrafficLaneTrafficLight), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(TrafficLaneCars), .inout = EcsInOut }
        },
        .callback = trafficCars_laneInitiateEndOfLaneBehavior
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneSetEndOfLaneState",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLane), .inout = EcsIn },
            { .id = ecs_id(TrafficLaneTrafficLight), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(TrafficLaneCars), .inout = EcsInOut }
        },
        .callback = trafficCars_laneSetEndOfLaneState
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneCarSetDrivingState",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLane), .inout = EcsIn },
            { .id = ecs_id(TrafficLaneCars), .inout = EcsInOut }
        },
        .callback = trafficCars_laneCarSetDrivingState
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneAccelerateCars",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLaneCars), .inout = EcsInOut }
        },
        .callback = trafficCars_laneAccelerateCars
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneUpdateCarEntities",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLane), .inout = EcsIn },
            { .id = ecs_id(TrafficLaneCars), .inout = EcsIn },
            { .id = ecs_id(TrafficLaneCarEntities), .inout = EcsIn },
            { .id = ecs_id(FlecsWorldTransform3), .inout = EcsInOut },
            { .id = ecs_id(TrafficCorner), .inout = EcsIn,
              .src.id = EcsSelf, .oper = EcsOptional }
        },
        .callback = trafficCars_laneUpdateCarEntities
    });

    ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "LaneMoveCarsToNextLane",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))
        }),
        .query.terms = {
            { .id = ecs_id(TrafficLane), .inout = EcsInOut },
            { .id = ecs_id(TrafficLaneCars), .inout = EcsInOut },
            { .id = ecs_id(TrafficLaneCarEntities), .inout = EcsInOut },
            { .id = ecs_id(TrafficLaneTrafficLight), .inout = EcsIn,
              .oper = EcsOptional }
        },
        .callback = trafficCars_laneMoveCarsToNextLane
    });
}
