#include "sdkconfig.h"
#include "middleware.h"

#if CONFIG_ROBOT_MIDDLEWARE_MICROROS
#include "microros_node.h"

#define MW_BACKEND "microros"
#define MW_LABEL "micro-ROS"
#define MW_LIBRARY "micro-ROS / XRCE-DDS"

void RobotMiddlewareStart(void)
{
    MicrorosStart();
}
#elif CONFIG_ROBOT_MIDDLEWARE_ZENOH
#include "zenoh_node.h"

#define MW_BACKEND "zenoh"
#define MW_LABEL "Zenoh"
#define MW_LIBRARY "zenoh-pico 1.9.0"

void RobotMiddlewareStart(void)
{
    ZenohStart();
}
#else
#define MW_BACKEND "none"
#define MW_LABEL "未启用"
#define MW_LIBRARY ""

void RobotMiddlewareStart(void)
{
    // No local middleware selected.
}
#endif

const char *TankMiddlewareBackend(void)
{
    return MW_BACKEND;
}

const char *TankMiddlewareLabel(void)
{
    return MW_LABEL;
}

const char *TankMiddlewareLibrary(void)
{
    return MW_LIBRARY;
}
