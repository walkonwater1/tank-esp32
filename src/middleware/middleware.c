#include "sdkconfig.h"
#include "middleware.h"

#if CONFIG_ROBOT_MIDDLEWARE_MICROROS
#include "microros_node.h"

void RobotMiddlewareStart(void)
{
    MicrorosStart();
}
#elif CONFIG_ROBOT_MIDDLEWARE_ZENOH
#include "zenoh_node.h"

void RobotMiddlewareStart(void)
{
    ZenohStart();
}
#else
void RobotMiddlewareStart(void)
{
    // No local middleware selected.
}
#endif
