# Navigation Lite
A lightweight Navigation stack for the drone, with full 3D navigation.  Built on the architecture of the Navigation2 stack.  I developed this one to learn how to. Much of this code can be used to build plugins for the Navigation2 stack.  The big deviation from Nav2 is that this stack works in three dimentional navigation. Launch the navigation stack like so:

```
ros2 launch navigation_lite navigation_lite.launch.py
```

## Navigation Server
Main interface waiting for messages nav_lite/navigate_to_pose to fly from point A to point B.  This uses a Behviour Tree to achieve the mission.  It is assumed that all the coordinates is in the map frame, NEU orientation, with angles in radians.  Coordinates are provided in the "map" frame.

## Controller Server
Will move the drone along a collection of waypoints (typically the output of the planner server), using obstacle avoidance and a local planner. Obstacle avoidance is achieved by testing the path to the next waypoint against the latest published map.  The past has to be free space.  If the path is not free space, it implies that an obstacle has moved accros the path (and was detected by a sensor).  The acton server should then stop motion, and return unsuccessfull (A number of waypoints were not reached).
The local planner in this node needs upgrading.  Current research suggests that Bezier points will be plotted through the provided waypoints to provide a smooth flight trajectory.  This is work in progress and the field of current research (14 April 2022)

## Recovery Server
Executes recovery actions.  Recivery action implimented are wait and spin.  Spin will rotate the drone at the current altitude a set arc.  Hopefully this gives the sensors time to inform the map server of any obstacles.  A wait recovery is also available.  This will maintain altitude and position for a set duration of time.  This might allow an obstacle (the dog for instance) to move along, and the sensors to detect a clear path again. Future recovery actions could inclue to change the altitude x meters.  

## Planner Server
Reads a Octree Map from the Map Server (external server using the [ROS2 Octomap Server](https://github.com/OctoMap/octomap_mapping) )and calculates a global flight plan.  Returns a sequence of waypoints for the Controller Server to follow. 

This server uses plugins. It is important that you install a suitable plugin package.  See [Planner Plugins] (https://github.com/slaghuis/planner_plugins) for a well coded Theta Star algoritm.  Other plugins are also available. 

## Dependancies
This package needs a Octomap server [ROS2 Octomap Server](https://github.com/OctoMap/octomap_mapping).  The Octomap server needs sensor_msgs::msg::PointCloud publisher to build the map. (One could also load a static map, bat that is noo fun).  To asist, one could run the [Sensor PointCloud](https://github.com/slaghuis/sensor_pointcloud) package to read one or more range sensors, and publish the data in a point cloud.  One day someone will sponsor me a Intel Realsense camera for my drone.

The planner server also requires a plugin to funciton.  See [Planner Plugins](https://github.com/slaghuis/planner_plugins)

This package depend on the [Navigation Lite Interfaces](https://github.com/slaghuis/navigation_interfaces) for message definitions.

# Code Status
This code has flown on a drone!  The drone was controlled by a Pixhawk mini 4.0 controlled via a Raspberry Pi 4 4Gb companion computer via UART.  This Navigation stack used the [drone_mavsdk](https://github.com/slaghuis/drone_mavsdk) node to effect the movement.  The mission was controlled via the [flight_control](https://github.com/slaghuis/flight_control) node.
