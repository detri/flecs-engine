#ifndef TRAFFIC_CARS_H
#define TRAFFIC_CARS_H

#include <flecs.h>
#include <flecs_engine.h>

#define TRAFFIC_MAX_CARS_PER_LANE 8

#define TrafficCarStateUnknown        0
#define TrafficCarStateAccelerating   1
#define TrafficCarStateDriving        2
#define TrafficCarStateBreaking       3
#define TrafficCarStateBreakingHard   4
#define TrafficCarStateStopped        5
#define TrafficCarStateCrashed        6

#define TrafficEolDefault                       0
#define TrafficEolReserveIntersection           1
#define TrafficEolWaitForIntersection           2
#define TrafficEolMoveOnIntersection            3
#define TrafficEolOnIntersection                4
#define TrafficEolWaitForProtectedIntersection  5
#define TrafficEolMoveOnProtectedIntersection   6
#define TrafficEolOnProtectedIntersection       7

#define TrafficDirTop    0
#define TrafficDirRight  1
#define TrafficDirBottom 2
#define TrafficDirLeft   3

#define TrafficConnTopToBottom    0
#define TrafficConnLeftToRight    1
#define TrafficConnTopToLeft      2
#define TrafficConnTopToRight     3
#define TrafficConnBottomToRight  4
#define TrafficConnBottomToLeft   5

typedef struct {
    float position;
    float length;
    float mass;
    float speed;
    float target_speed;
    uint8_t state;
    uint8_t eol_state;
    uint8_t reservation;
    uint8_t wait_count;
    ecs_entity_t next_lane;
} TrafficCar;

extern ECS_COMPONENT_DECLARE(TrafficCar);

typedef struct {
    TrafficCar cars[TRAFFIC_MAX_CARS_PER_LANE];
    int8_t count;
} TrafficLaneCars;

extern ECS_COMPONENT_DECLARE(TrafficLaneCars);

typedef struct {
    ecs_entity_t cars[TRAFFIC_MAX_CARS_PER_LANE];
} TrafficLaneCarEntities;

extern ECS_COMPONENT_DECLARE(TrafficLaneCarEntities);

typedef struct {
    float length;
    float width;
    float max_speed;
    ecs_entity_t next;
    ecs_entity_t road;
} TrafficLane;

extern ECS_COMPONENT_DECLARE(TrafficLane);

typedef struct {
    float radius;
    bool invert_direction;
} TrafficCorner;

extern ECS_COMPONENT_DECLARE(TrafficCorner);

typedef struct {
    ecs_entity_t road;
    int8_t edge;
} TrafficRoadConnect;

extern ECS_COMPONENT_DECLARE(TrafficRoadConnect);

typedef struct {
    float length;
    float lane_width;
    float max_speed;
    bool corner;
    bool invert_corner;
    TrafficRoadConnect next;
    ecs_entity_t intersection;
} TrafficRoad;

extern ECS_COMPONENT_DECLARE(TrafficRoad);

typedef struct {
    ecs_entity_t lanes[2];
} TrafficRoadLanes;

extern ECS_COMPONENT_DECLARE(TrafficRoadLanes);

typedef struct {
    TrafficRoadConnect roads[4];
    float lane_width;
    float max_speed;
} TrafficIntersection;

extern ECS_COMPONENT_DECLARE(TrafficIntersection);

typedef struct {
    ecs_entity_t roads[6];
    ecs_entity_t movements[4];
    uint8_t current_reservation;
    uint8_t next_reservation;
} TrafficIntersectionRoads;

extern ECS_COMPONENT_DECLARE(TrafficIntersectionRoads);

typedef struct {
    ecs_entity_t intersection;
    ecs_entity_t lanes[3];
} TrafficIntersectionMovement;

extern ECS_COMPONENT_DECLARE(TrafficIntersectionMovement);

extern ECS_DECLARE(TrafficCarRoot);
extern ECS_DECLARE(TrafficRoadRoot);

void TrafficCarsImport(ecs_world_t *world);

void trafficCars_finalizeScene(ecs_world_t *world);

void trafficCars_addCarToLane(
    ecs_world_t *world,
    ecs_entity_t out_lane,
    ecs_entity_t car_entity,
    float position,
    float speed,
    float target_speed,
    uint8_t state,
    uint8_t eol_state,
    uint8_t reservation);

ecs_entity_t trafficCars_intersectionFromLane(
    ecs_world_t *world,
    ecs_entity_t lane);

#endif
