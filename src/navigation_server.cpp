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
 * An Action Server Node that forms the main interface to the navigation 
 * stack.  It executes a behaviour tree specified in the Action Server 
 * goal (xml file name).  The action server serves AsyncActionNodes that
 * call Planner, Controller and Recovery Action Servers and Simple Services
 * to move the drone safely in 3D space.
 *
 * Transform listener tf2 for odom->base_link transforms.
 * **********************************************************************/

#include <functional>
#include <memory>
#include <thread>

#include "builtin_interfaces/msg/duration.hpp"
#include "geometry_msgs/msg/pose_stamped.hpp"
#include "navigation_interfaces/action/navigate_to_pose.hpp"

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "rclcpp_components/register_node_macro.hpp"

#include <tf2/exceptions.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/buffer.h>

#include "navigation_lite/visibility_control.h"

#include "navigation_lite/action_wait.h"
#include "navigation_lite/action_spin.h"
#include "navigation_lite/action_follow_waypoints.h"
#include "navigation_lite/action_compute_path_to_pose.h"

using namespace std::chrono_literals;
using namespace BT;

namespace navigation_lite
{
class NavigationServer : public rclcpp::Node
{
public:
  using NavigateToPose = navigation_interfaces::action::NavigateToPose;
  using GoalHandleNavigateToPose = rclcpp_action::ServerGoalHandle<NavigateToPose>;

  NAVIGATION_LITE_PUBLIC
  explicit NavigationServer(const rclcpp::NodeOptions & options = rclcpp::NodeOptions())
  : Node("navigation_server", options)
  {
    using namespace std::placeholders;
    
    // Create a transform listener
    tf_buffer_ =
      std::make_unique<tf2_ros::Buffer>(this->get_clock());
    transform_listener_ =
      std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

    // Call on_timer function every half a second (Is this enouh to ensure smooth motion?
    timer_ = this->create_wall_timer(
      500ms, std::bind(&NavigationServer::on_timer, this));
    // RCLCPP_INFO(this->get_logger(), "Transform Listener [odom->base_link] started");

    this->action_server_ = rclcpp_action::create_server<NavigateToPose>(
      this,
      "nav_lite/navigate_to_pose",
      std::bind(&NavigationServer::handle_goal, this, _1, _2),
      std::bind(&NavigationServer::handle_cancel, this, _1),
      std::bind(&NavigationServer::handle_accepted, this, _1));
      RCLCPP_INFO(this->get_logger(), "Action Serever [nav_lite/navigate_to_pose] started");
  }

private:
  rclcpp::TimerBase::SharedPtr timer_{nullptr};
  std::shared_ptr<tf2_ros::TransformListener> transform_listener_{nullptr};
  std::unique_ptr<tf2_ros::Buffer> tf_buffer_;

  // Last (transformed) pose of the drone
  std::shared_ptr<geometry_msgs::msg::Pose> last_pose = std::make_shared<geometry_msgs::msg::Pose>();
  rclcpp_action::Server<NavigateToPose>::SharedPtr action_server_;
  

  rclcpp_action::GoalResponse handle_goal(
    const rclcpp_action::GoalUUID & uuid,
    std::shared_ptr<const NavigateToPose::Goal> goal)
  {
    RCLCPP_INFO(this->get_logger(), "Received request with behaviour tree %s",goal->behavior_tree.c_str());
    RCLCPP_INFO(this->get_logger(), "Received goal request to fly to [%.2f; %.2f; %.2f]", goal->pose.pose.position.x, goal->pose.pose.position.z, goal->pose.pose.position.z);
    (void)uuid;
    return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
  }

  rclcpp_action::CancelResponse handle_cancel(
    const std::shared_ptr<GoalHandleNavigateToPose> goal_handle)
  {
    RCLCPP_INFO(this->get_logger(), "Received request to cancel goal");
    (void)goal_handle;
    return rclcpp_action::CancelResponse::ACCEPT;
  }

  void handle_accepted(const std::shared_ptr<GoalHandleNavigateToPose> goal_handle)
  {
    using namespace std::placeholders;
    // this needs to return quickly to avoid blocking the executor, so spin up a new thread
    std::thread{std::bind(&NavigationServer::execute, this, _1), goal_handle}.detach();
  }

  void execute(const std::shared_ptr<GoalHandleNavigateToPose> goal_handle)
  {
    RCLCPP_INFO(this->get_logger(), "Executing goal");
    rclcpp::Rate loop_rate(1);
    const auto goal = goal_handle->get_goal();
    const auto wp = goal->pose;
    auto feedback = std::make_shared<NavigateToPose::Feedback>();
    auto & current_pose = feedback->current_pose;
    auto & navigation_time = feedback->navigation_time;
    auto & estimated_time_remaining = feedback->estimated_time_remaining;
    auto & number_of_recoveries = feedback->number_of_recoveries;
    auto & distance_remaining = feedback->distance_remaining;
    auto result = std::make_shared<NavigateToPose::Result>();
  
    // This behaviorTree works with corourines.
    // SEE https://github.com/BehaviorTree/BehaviorTree.CPP/blob/master/examples/t09_async_actions_coroutines.cpp

    BehaviorTreeFactory factory;
    Tree tree;
    using namespace NavigationNodes;
    
    factory.registerNodeType<NavLiteWaitAction>("Wait");
    factory.registerNodeType<NavLiteSpinAction>("Spin");
    factory.registerNodeType<NavLiteFollowWaypointsAction>("FollowWaypoints");
    factory.registerNodeType<NavLiteComputePathToPoseAction>("ComputePathToPose");
    
    tree = factory.createTreeFromFile(goal->behavior_tree);
    RCLCPP_INFO(this->get_logger(), "Tree Loaded");
    
    // Initialise the BT Nodes
    auto node_ptr = shared_from_this();                
    // Iterate through all the nodes and call init() if it is an Action_B
    for( auto& node: tree.nodes )
    {
      // Not a typo: it is "=", not "=="
      if( auto wait_action = dynamic_cast<NavLiteWaitAction *>( node.get() ))
      {
        wait_action->init( node_ptr );
      } else if( auto spin_action = dynamic_cast<NavLiteSpinAction *>( node.get() ))
      {
        spin_action->init( node_ptr );
      } else if( auto follow_waypoints_action = dynamic_cast<NavLiteFollowWaypointsAction *>( node.get() ))
      {
        follow_waypoints_action->init( node_ptr );
      } else if( auto compute_path_to_pose_action = dynamic_cast<NavLiteComputePathToPoseAction *>( node.get() ))
      {
        compute_path_to_pose_action->init( node_ptr );
      }
    }
        
    auto start_time = now();
    
    while( ( tree.tickRoot() == NodeStatus::RUNNING) && rclcpp::ok() )
    {
      // Check if there is a cancel request
      if (goal_handle->is_canceling()) {
        // Result is std_msgs/Empty.  Send nothing
        goal_handle->canceled(result);
        RCLCPP_INFO(this->get_logger(), "Goal canceled");
        return;
      }
      
      // Publish Feedback
      current_pose.pose.position.x = last_pose->position.x;        // geometry_msgs/PoseStamped current_pose
      current_pose.pose.position.y = last_pose->position.y;
      current_pose.pose.position.z = last_pose->position.z;
      
      current_pose.pose.orientation.x = last_pose->orientation.x;
      current_pose.pose.orientation.y = last_pose->orientation.y;
      current_pose.pose.orientation.z = last_pose->orientation.z;
      current_pose.pose.orientation.w = last_pose->orientation.w;
      
      navigation_time = now() - start_time;                         // builtin_interfaces/Duration navigation_time
      estimated_time_remaining.sec = 0;                             // builtin_interfaces/Duration estimated_time_remaining 
      estimated_time_remaining.nanosec = 0;
      number_of_recoveries = 0;                                     // int16 number_of_recoveries
      
      auto err_x = wp.pose.position.x - last_pose->position.x; 
      auto err_y = wp.pose.position.y - last_pose->position.y; 
      auto xy_distance = sqrt(pow(err_x,2) + pow(err_y,2));
      auto err_z = wp.pose.position.y - last_pose->position.z; 
      distance_remaining = sqrt(pow(err_z,2) + pow(xy_distance,2)); // float32 distance_remaining
      
      goal_handle->publish_feedback(feedback);
      
      loop_rate.sleep();
    }

    
    // Check if goal is done
    if (rclcpp::ok()) {
      goal_handle->succeed(result);
      RCLCPP_INFO(this->get_logger(), "Goal succeeded");
    }
  }
  
  // Transformation Listener/////////////////////////////////////////////////////
  void on_timer()
  {
    // Store frame names in variables that will be used to
    // compute transformations

    std::string source_frameid = "odom";
    std::string target_frameid = "base_link";

    geometry_msgs::msg::TransformStamped transformStamped;

    // Look up for the transformation between odom and base_link frames
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
    
  }
};  // class NavigationServer

}  // namespace navigation_lite

RCLCPP_COMPONENTS_REGISTER_NODE(navigation_lite::NavigationServer)