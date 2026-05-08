#ifndef TRAFFIC_CARS_H
#define TRAFFIC_CARS_H

#include <flecs.h>
#include <flecs_engine.h>

#define TRAFFIC_MAX_CARS_PER_LANE 8

extern uint8_t TrafficLightGreenTicks;
extern uint8_t TrafficLightOrangeTicks;
extern float TrafficAccelerationForce;
extern float TrafficBreakForce;
extern float TrafficHardBreakForce;
extern uint8_t TrafficMaxWaitCount;
extern float TrafficPlaceholderCarMass;
extern float TrafficPlaceholderCarLength;

typedef enum TrafficDirection {
    TrafficTop = 0,
    TrafficRight = 1,
    TrafficBottom = 2,
    TrafficLeft = 3
} TrafficDirection;

typedef enum TrafficConnection {
    TrafficTopToBottom = 0,
    TrafficLeftToRight = 1,
    TrafficTopToLeft = 2,
    TrafficTopToRight = 3,
    TrafficBottomToRight = 4,
    TrafficBottomToLeft = 5
} TrafficConnection;

typedef struct TrafficLight {
    int8_t state;
} TrafficLight;

extern ECS_COMPONENT_DECLARE(TrafficLight);

typedef enum TrafficCarState {
    TrafficCarStateUnknown,
    TrafficCarStateAccelerating,
    TrafficCarStateDriving,
    TrafficCarStateBreaking,
    TrafficCarStateBreakingHard,
    TrafficCarStateStopped,
    TrafficCarStateCrashed
} TrafficCarState;

typedef enum TrafficCarEndOfLaneState {
    TrafficCarEolDefault,
    TrafficCarEolReserveIntersection,
    TrafficCarEolWaitForIntersection,
    TrafficCarEolMoveOnIntersection,
    TrafficCarEolOnIntersection,
    TrafficCarEolWaitForProtectedIntersection,
    TrafficCarEolMoveOnProtectedIntersection,
    TrafficCarEolOnProtectedIntersection
} TrafficCarEndOfLaneState;

typedef struct TrafficCar {
    float position;
    float length;
    float mass;
    float speed;
    float target_speed;
    TrafficCarState state;
    TrafficCarEndOfLaneState eol_state;
    uint8_t reservation;
    uint8_t wait_count;
    ecs_entity_t next_lane;
} TrafficCar;

extern ECS_COMPONENT_DECLARE(TrafficCar);

typedef struct TrafficLaneCars {
    TrafficCar cars[TRAFFIC_MAX_CARS_PER_LANE];
    int8_t count;
} TrafficLaneCars;

extern ECS_COMPONENT_DECLARE(TrafficLaneCars);

typedef struct TrafficLaneCarEntities {
    ecs_entity_t cars[TRAFFIC_MAX_CARS_PER_LANE];
} TrafficLaneCarEntities;

extern ECS_COMPONENT_DECLARE(TrafficLaneCarEntities);

typedef struct TrafficLane {
    float length;
    float width;
    float max_speed;
    ecs_entity_t next;
    ecs_entity_t road;
} TrafficLane;

extern ECS_COMPONENT_DECLARE(TrafficLane);

typedef enum TrafficLaneTrafficLightState {
    TrafficLaneTrafficLightDefault,
    TrafficLaneTrafficLightReserved,
    TrafficLaneTrafficLightAcquired,
    TrafficLaneTrafficLightReleasing
} TrafficLaneTrafficLightState;

typedef struct TrafficLaneTrafficLight {
    TrafficLaneTrafficLightState state;
    uint8_t reservation;
    uint8_t timer;
    ecs_entity_t light;
} TrafficLaneTrafficLight;

extern ECS_COMPONENT_DECLARE(TrafficLaneTrafficLight);

typedef struct TrafficCorner {
    float radius;
    bool invert_direction;
} TrafficCorner;

extern ECS_COMPONENT_DECLARE(TrafficCorner);

typedef struct TrafficRoadConnect {
    ecs_entity_t road;
    int8_t edge;
} TrafficRoadConnect;

extern ECS_COMPONENT_DECLARE(TrafficRoadConnect);

typedef struct TrafficRoad {
    float length;
    float lane_width;
    float max_speed;
    bool corner;
    bool invert_corner;
    TrafficRoadConnect next;
    ecs_entity_t intersection;
} TrafficRoad;

extern ECS_COMPONENT_DECLARE(TrafficRoad);

typedef struct TrafficRoadLanes {
    ecs_entity_t lanes[2];
} TrafficRoadLanes;

extern ECS_COMPONENT_DECLARE(TrafficRoadLanes);

typedef struct TrafficIntersection {
    TrafficRoadConnect roads[4];
    float lane_width;
    float max_speed;
} TrafficIntersection;

extern ECS_COMPONENT_DECLARE(TrafficIntersection);

typedef struct TrafficIntersectionRoads {
    ecs_entity_t roads[6];
    ecs_entity_t movements[4];
    uint8_t current_reservation;
    uint8_t next_reservation;
} TrafficIntersectionRoads;

extern ECS_COMPONENT_DECLARE(TrafficIntersectionRoads);

typedef struct TrafficIntersectionMovement {
    ecs_entity_t intersection;
    ecs_entity_t lanes[3];
} TrafficIntersectionMovement;

extern ECS_COMPONENT_DECLARE(TrafficIntersectionMovement);

extern ECS_DECLARE(TrafficCarRoot);
extern ECS_DECLARE(TrafficRoadRoot);

void TrafficCarsImport(ecs_world_t *world);

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
    uint8_t reservation);

ecs_entity_t trafficCars_roadFromLane(
    ecs_world_t *world,
    ecs_entity_t lane);

ecs_entity_t trafficCars_intersectionFromLane(
    ecs_world_t *world,
    ecs_entity_t lane);

#endif
