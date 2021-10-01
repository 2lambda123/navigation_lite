// Copyright (c) 2021 Xeni Robotics
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/* **********************************************************************
 * Subscribe to tf2 odom->base_link for position and pose data
 * Subscribe to sensor_msgs/msg/Range
 *
 * Action Server responding to navgation_interfaces/action/FollowWaypoints
 *   called only by the Navigation Server
 * Publishes cmd_vel as geometry_msgs/msg/Twist to effect motion.
 * Motion would be:
 *   - Amend yaw, to point to the next waypoint
 *   - Increase foreward velocity to reach desitnation, using a PID 
 *       controller to govern speed
 *   - If an obstacle is encountered, stop and fail (requesting recovery)
 *
 * A Mutex lock governs that only one action server can control the drone
 *  at a time.  Who knows what would happen if another node starts sending 
 *  out cmd_vel messages?
 *
 * ***********************************************************************/
#include <functional>
#include <memory>
#include <thread>
#include <mutex>

#include "builtin_interfaces/msg/duration.hpp"
#include "geometry_msgs/msg/pose_stamped.hpp"
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <geometry_msgs/msg/twist.hpp>

#include "navigation_interfaces/action/follow_waypoints.hpp"
#include "navigation_interfaces/action/spin.hpp"
#include "navigation_interfaces/action/wait.hpp"

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "rclcpp_components/register_node_macro.hpp"

#include <tf2/exceptions.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/buffer.h>

#include "navigation_lite/visibility_control.h"
#include "navigation_lite/pid.h"

static const float DEFAULT_MAX_SPEED_XY = 0.25;         // Maximum horizontal speed, in m/s
static const float DEFAULT_MAX_SPEED_Z = 0.33;          // Maximum vertical speed, in m/s
static const float DEFAULT_MAX_YAW_SPEED = 50.0;        // Maximum yaw speed in radians/s  (Really not radians.  More like degrees)
static const float DEFAULT_WAYPOINT_RADIUS_ERROR = 0.3; // Acceptable XY distance to waypoint deemed as close enough

inline double getAbsoluteDiff2Angles(const double x, const double y, const double c)
{
    // c can be PI (for radians) or 180.0 (for degrees);
    return c - fabs(fmod(fabs(x - y), 2*c) - c);
}

namespace navigation_lite
{
class ControllerServer : public rclcpp::Node
{
public:
  using FollowWaypoints = navigation_interfaces::action::FollowWaypoints;
  using GoalHandleFollowWaypoints = rclcpp_action::ServerGoalHandle<FollowWaypoints>;

  NAVIGATION_LITE_PUBLIC
  explicit ControllerServer(const rclcpp::NodeOptions & options = rclcpp::NodeOptions())
  : Node("controller_server", options)
  {            
    one_off_timer_ = this->create_wall_timer(
      1000ms, std::bind(&ControllerServer::init, this));
  }
    
private:    
  std::mutex server_mutex;   // Only allow one Action Server to address the drone at a time
  
  // Node Parameters
  float max_yaw_speed_;
  float max_speed_xy_;
  float max_speed_z_;
  float waypoint_radius_error_;
  
  // Clock
  rclcpp::Clock steady_clock_{RCL_STEADY_TIME};

  rclcpp::TimerBase::SharedPtr one_off_timer_;

  rclcpp::TimerBase::SharedPtr timer_{nullptr};
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr publisher_;
  std::shared_ptr<tf2_ros::TransformListener> transform_listener_{nullptr};
  std::unique_ptr<tf2_ros::Buffer> tf_buffer_;

  // Last (transformed) pose of the drone
  std::shared_ptr<geometry_msgs::msg::Pose> last_pose = std::make_shared<geometry_msgs::msg::Pose>();
  double last_yaw;
  
  // PID Controllers  
  std::shared_ptr<PID> pid_x;
  std::shared_ptr<PID> pid_z;
  std::shared_ptr<PID> pid_yaw;

  void init() {
    using namespace std::placeholders;
    
    // Only run this once.  Stop the timer that triggered this.
    this->one_off_timer_->cancel();
       
    // Declare node parameters    
    this->declare_parameter<float>("max_speed_xy", DEFAULT_MAX_SPEED_XY);
    this->declare_parameter<float>("max_speed_z", DEFAULT_MAX_SPEED_Z);
    this->declare_parameter<float>("max_yaw_speed", DEFAULT_MAX_YAW_SPEED);
    this->declare_parameter("pid_xy", std::vector<double>{0.7, 0.0, 0.0});
    this->declare_parameter("pid_z", std::vector<double>{0.7, 0.0, 0.0});
    this->declare_parameter("pid_yaw", std::vector<double>{0.7, 0.0, 0.0});    
    this->declare_parameter<float>("waypoint_radius_error", DEFAULT_WAYPOINT_RADIUS_ERROR);

    // Read the parameters
    this->get_parameter("max_yaw_speed", max_yaw_speed_);
    this->get_parameter("max_speed_xy", max_speed_xy_);
    this->get_parameter("max_speed_z", max_speed_z_);
    this->get_parameter("waypoint_radius_error", waypoint_radius_error_);
    
    rclcpp::Parameter pid_xy_settings_param = this->get_parameter("pid_xy");
    std::vector<double> pid_xy_settings = pid_xy_settings_param.as_double_array(); 
    pid_x   = std::make_shared<PID>(0.5, max_speed_xy_, -max_speed_xy_, (float)pid_xy_settings[0], (float)pid_xy_settings[1], (float)pid_xy_settings[2]);

    rclcpp::Parameter pid_z_settings_param = this->get_parameter("pid_z");
    std::vector<double> pid_z_settings = pid_z_settings_param.as_double_array(); 
    pid_z   = std::make_shared<PID>(0.5, max_speed_z_, -max_speed_z_, (float)pid_z_settings[0], (float)pid_z_settings[1], (float)pid_z_settings[2]);

    rclcpp::Parameter pid_yaw_settings_param = this->get_parameter("pid_yaw");
    std::vector<double> pid_yaw_settings = pid_yaw_settings_param.as_double_array(); 
    pid_yaw   = std::make_shared<PID>(0.5, max_yaw_speed_, -max_yaw_speed_, (float)pid_yaw_settings[0], (float)pid_yaw_settings[1], (float)pid_yaw_settings[2]);


    // Create a transform listener
    tf_buffer_ =
      std::make_unique<tf2_ros::Buffer>(this->get_clock());
    transform_listener_ =
      std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

    // Call on_timer function every half a second (Is this enough to ensure smooth motion?
    timer_ = this->create_wall_timer(
      500ms, std::bind(&ControllerServer::on_timer, this));
    RCLCPP_DEBUG(this->get_logger(), "Transform Listener [map->base_link] started");  

    // Create drone velocity publisher
    publisher_ =
      this->create_publisher<geometry_msgs::msg::Twist>("drone/cmd_vel", 1);

    // Create the action server
    this->action_server_ = rclcpp_action::create_server<FollowWaypoints>(
      this,
      "nav_lite/follow_waypoints",
      std::bind(&ControllerServer::handle_goal, this, _1, _2),
      std::bind(&ControllerServer::handle_cancel, this, _1),
      std::bind(&ControllerServer::handle_accepted, this, _1));
    RCLCPP_INFO(this->get_logger(), "Action Server [nav_lite/follow_waypoints] started");
    
  }   

// Transformation Listener/////////////////////////////////////////////////////
  void on_timer()
  {
    // Store frame names in variables that will be used to
    // compute transformations

    std::string source_frameid = "map";
    std::string target_frameid = "base_link";

    geometry_msgs::msg::TransformStamped transformStamped;

    // Look up for the transformation between map and base_link frames
    // and save the last position
    try {
      transformStamped = tf_buffer_->lookupTransform(
        target_frameid, source_frameid,
        tf2::TimePointZero);
    } catch (tf2::TransformException & ex) {
      RCLCPP_DEBUG(
        this->get_logger(), "Could not transform %s to %s: %s",
        target_frameid.c_str(), source_frameid.c_str(), ex.what());
      return;
    }

    last_pose->position.x = transformStamped.transform.translation.x;  // Foreward of origin
    last_pose->position.y = transformStamped.transform.translation.y;  // Left of origin
    last_pose->position.z = transformStamped.transform.translation.z;  // Above origin
    
    last_pose->orientation.x = transformStamped.transform.rotation.x;  // Quaterion
    last_pose->orientation.y = transformStamped.transform.rotation.y;
    last_pose->orientation.z = transformStamped.transform.rotation.z;
    last_pose->orientation.w = transformStamped.transform.rotation.w;
    
    // Calculate yaw
    // Orientation quaternion
    tf2::Quaternion q(
      last_pose->orientation.x,
      last_pose->orientation.y,
      last_pose->orientation.z,
      last_pose->orientation.w);

    // 3x3 Rotation matrix from quaternion
    tf2::Matrix3x3 m(q);

    // Roll Pitch and Yaw from rotation matrix
    double roll, pitch;
    m.getRPY(roll, pitch, last_yaw);

  }
  
// FollowWayPoint Action Server /////////////////////////////////////////////////////////////////
  rclcpp_action::Server<FollowWaypoints>::SharedPtr action_server_;
  rclcpp_action::GoalResponse handle_goal(
    const rclcpp_action::GoalUUID & uuid,
    std::shared_ptr<const FollowWaypoints::Goal> goal)
  {
    RCLCPP_INFO(this->get_logger(), "Received request to follow %d waypoints", goal->poses.size());
    (void)uuid;
    if(server_mutex.try_lock()) {
      return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    } else {
      RCLCPP_ERROR(this->get_logger(), "Another thread is commanding the drone now.  Rejecting request.");
      return rclcpp_action::GoalResponse::REJECT; 
    }  
  }

  rclcpp_action::CancelResponse handle_cancel(
    const std::shared_ptr<GoalHandleFollowWaypoints> goal_handle)
  {
    RCLCPP_INFO(this->get_logger(), "Received request to cancel goal");
    (void)goal_handle;
    return rclcpp_action::CancelResponse::ACCEPT;
  }

  void handle_accepted(const std::shared_ptr<GoalHandleFollowWaypoints> goal_handle)
  {
    using namespace std::placeholders;
    // this needs to return quickly to avoid blocking the executor, so spin up a new thread
    std::thread{std::bind(&ControllerServer::execute, this, _1), goal_handle}.detach();
  }

  void execute(const std::shared_ptr<GoalHandleFollowWaypoints> goal_handle)
  {
    RCLCPP_INFO(this->get_logger(), "Executing goal");
    rclcpp::Rate loop_rate(2);
    const auto goal = goal_handle->get_goal();
    auto feedback = std::make_shared<FollowWaypoints::Feedback>();
    auto & current_waypoint = feedback->current_waypoint;
    auto result = std::make_shared<FollowWaypoints::Result>();
    geometry_msgs::msg::Twist setpoint = geometry_msgs::msg::Twist();
            
    RCLCPP_INFO(this->get_logger(), "Received %d waypoints.", goal->poses.size());        
    current_waypoint = 0;
    for (geometry_msgs::msg::PoseStamped wp : goal->poses ) {
       
      if (!rclcpp::ok()) {
        // Something is amis. Record some feedback and break out of the loop.
        for( long unsigned int i = current_waypoint; i < goal->poses.size(); i++) {        
          result->missed_waypoints.push_back(i);
        }
        break;
      }
            
      double err_x = wp.pose.position.x - last_pose->position.x; 
      double err_y = wp.pose.position.y - last_pose->position.y;
      double distance = sqrt(pow(err_x,2) + pow(err_y,2));
    
      // Navigate the drone to this waypoint.
      bool waypoint_is_close = (distance < waypoint_radius_error_);      
      while ( (!waypoint_is_close) && rclcpp::ok()) {
        RCLCPP_INFO(this->get_logger(), "Waypoint is NOT close. [%.1fm]", distance); 
        // Check if there is a cancel request
        if (goal_handle->is_canceling()) {
          for( long unsigned int i = current_waypoint; i < goal->poses.size(); i++) {        
            result->missed_waypoints.push_back(i);
          }
          goal_handle->canceled(result);
          RCLCPP_INFO(this->get_logger(), "Goal canceled");
          server_mutex.unlock();
          return;
        }

        err_x = wp.pose.position.x - last_pose->position.x; 
        err_y = wp.pose.position.y - last_pose->position.y; 
        distance = sqrt(pow(err_x,2) + pow(err_y,2));
        
        waypoint_is_close = (distance < waypoint_radius_error_);
      
        // Calculate direction (desired yaw angle in radians) to the landing target
        // NOTE:  -M_PI <= yaw <= M_PI
        double yaw_to_target = (err_x == 0.0) ? 0.0 : atan(err_y / err_x);
        if (err_x > 0.0) {
          if(err_y > 0.0) {
            yaw_to_target -= M_PI;
          } else {
            yaw_to_target += M_PI;
          }
        }

        double yaw_error = getAbsoluteDiff2Angles(yaw_to_target, last_yaw, M_PI);

        setpoint.angular.x = 0.0;
        setpoint.angular.y = 0.0;
        setpoint.angular.z = pid_yaw->calculate( 0.0, -yaw_error);         // correct yaw error down to zero
        setpoint.linear.x = 0.0;
        setpoint.linear.y = 0.0;  
        setpoint.linear.z = pid_z->calculate(wp.pose.position.z, last_pose->position.z);  // correct altitude

        bool vehicle_pose_is_good = yaw_error < 0.087;   // 5 degree error is enough to start flying
        if( vehicle_pose_is_good ) {      
          RCLCPP_INFO(this->get_logger(), "Direction is GOOD, distance now %d", distance);
          // The PID will return a negative, as we are trying to close the distance down to 0.  (Unless we have overshot the target)
          // for that reason we send a negative distance (sign of a poorly tuned PID)
          setpoint.linear.x = pid_x->calculate(0.0, -distance);                  // fly closer to the target
        } else {
          RCLCPP_INFO(this->get_logger(), "Direction is NOT good.  Current %.2f, Target %.2f", last_yaw, yaw_to_target); 
        }
     
        // If obstacle detected, Emergency stop!  Cancel te mission.
        // TODO
        // setpoint.linear.x = 0, stop the drone!!!
        // Break out of loop, populate unreached waypoints

        // Send the drone foreward
        publisher_->publish(setpoint);        
;
        // Dont flood the flight controller
        loop_rate.sleep();
      } // End movement while(){} loop
      
      
      // Publish feedback
      goal_handle->publish_feedback(feedback);  // Current waypoint
      RCLCPP_INFO(this->get_logger(), "Publish feedback");
      
      loop_rate.sleep();
      current_waypoint++;
    }
    
    // Correct the yaw to that required in the last waypoint
    //pid_yaw.reset();
    // std::shared_ptr<PID> pid_yaw = std::make_shared<PID>(0.5, max_yaw_speed_, -max_yaw_speed_, 0.7, 0.00, 0);


    // Calculate the desired yaw
    
    // Orientation quaternion
    tf2::Quaternion q(
      goal->poses.back().pose.orientation.x,
      goal->poses.back().pose.orientation.y,
      goal->poses.back().pose.orientation.z,
      goal->poses.back().pose.orientation.w);        

    // 3x3 Rotation matrix from quaternion
    tf2::Matrix3x3 m(q);

    // Roll Pitch and Yaw from rotation matrix
    double roll, pitch, yaw;
    m.getRPY(roll, pitch, yaw);
    
    // Set up a message
    setpoint.angular.x = 0.0;
    setpoint.angular.y = 0.0;
    setpoint.angular.z = pid_yaw->calculate( yaw, last_yaw);         // correct yaw

    setpoint.linear.x = 0.0;
    setpoint.linear.y = 0.0;  
    setpoint.linear.z = 0.0; 

    while (rclcpp::ok() && (abs(setpoint.angular.z) > 0.02)) {
    
      if (!rclcpp::ok()) {
        // Something is amis. Record some feedback and break out of the loop.
        for( long unsigned int i = current_waypoint; i < goal->poses.size(); i++) {        
          result->missed_waypoints.push_back(i);
        }
        break;
      }
      
      // Check if there is a cancel request
      if (goal_handle->is_canceling()) {
        for( long unsigned int i = current_waypoint; i < goal->poses.size(); i++) {        
          result->missed_waypoints.push_back(i);
        }
        goal_handle->canceled(result);
        RCLCPP_INFO(this->get_logger(), "Goal canceled");
        server_mutex.unlock();
        return;
      }

      // Ask the drone to turn
      publisher_->publish(setpoint);        
        
      // Dont flood the flight controller
      loop_rate.sleep();
      setpoint.angular.z = pid_yaw->calculate( yaw, last_yaw);         // correct yaw      
    };

    // Stop flight
    // Set up a message
    setpoint.angular.x = 0.0;
    setpoint.angular.y = 0.0;
    setpoint.angular.z = 0.0;

    setpoint.linear.x = 0.0;
    setpoint.linear.y = 0.0;  
    setpoint.linear.z = 0.0;  
    publisher_->publish(setpoint);        
    loop_rate.sleep();
    publisher_->publish(setpoint); // Just to be sure :-)       

    // Check if goal is done
    if (rclcpp::ok()) {
      // no feedback to record.  The feedback is an empty vector
      goal_handle->succeed(result);
      RCLCPP_INFO(this->get_logger(), "Goal succeeded");
    }
    server_mutex.unlock();
  }
   
  
};  // class ControllerServer

}  // namespace navigation_lite

RCLCPP_COMPONENTS_REGISTER_NODE(navigation_lite::ControllerServer)
