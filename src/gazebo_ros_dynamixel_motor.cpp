/*
 * Copyright (c) 2014 Team DIANA
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *      * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *      * Neither the name of the <organization> nor the
 *      names of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY Antons Rebguns <email> ''AS IS'' AND ANY
 *  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL Antons Rebguns <email> BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **/

/*
 * \file  gazebo_ros_dynamixel_motor.cpp
 *
 * \brief A configurable plugin that controls one or more joint.
 *
 * \author Vincenzo Comito <clynamen@gmail.com>
 */

#include <algorithm>
#include <assert.h>
#include <functional>
#include <cmath>

#include <gazebo_plugins/gazebo_ros_dynamixel_motor.h>
#include <gazebo_plugins/motor_state.h>
#include <gazebo_plugins/gazebo_ros_utils.h>

#include <team_diana_lib/logging/logging.h>
#include <team_diana_lib/strings/strings.h>
#include <team_diana_lib/math/math.h>
#include <team_diana_lib/random/random.h>

#include <gazebo/math/gzmath.hh>
#include <sdf/sdf.hh>

#include <dynamixel_msgs/JointState.h>
//#include <dynamixel_controllers/SetTorque.h>
#include <dynamixel_controllers/SetTorqueLimit.h>
#include <dynamixel_controllers/TorqueEnable.h>
#include <dynamixel_controllers/SetSpeed.h>
//#include <dynamixel_controllers/SetThreshold.h>
#include <dynamixel_controllers/SetCompliancePunch.h>
#include <dynamixel_controllers/SetComplianceSlope.h>
#include <dynamixel_controllers/SetComplianceMargin.h>

#include <ros/ros.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <geometry_msgs/Twist.h>
#include <boost/bind.hpp>
#include <boost/thread/mutex.hpp>
#include <std_msgs/Float64.h>

using namespace Td;
using namespace std;

using MsgType = dynamixel_msgs::JointState;

namespace gazebo {

  const std::string GazeboRosDynamixelMotor::PLUGIN_NAME = "GazeboRosDynamixelMotor";

  GazeboRosDynamixelMotor::GazeboRosDynamixelMotor() : alive_(true) {}

  // Destructor
  GazeboRosDynamixelMotor::~GazeboRosDynamixelMotor() {
    delete rosnode;
  }

  // Load the controller
  void GazeboRosDynamixelMotor::Load(physics::ModelPtr parent, sdf::ElementPtr sdf) {
    using namespace std;
    using namespace sdf;

    this->parent = parent;
    this->world = parent->GetWorld();

    this->robot_namespace = GetValueFromElement<string>(sdf, "robotNamespace", "");

    joint = GetReferencedJoint(parent, sdf, "joint");

    if(joint == nullptr) {
      ros_fatal("No joint was found");
      return;
    }


    // Make sure the ROS node for Gazebo has already been initialized
    if (!ros::isInitialized())
    {
      ROS_FATAL_STREAM("A ROS node for Gazebo has not been initialized, unable to load plugin. "
        << "Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in the gazebo_ros package)");
      return;
    }

    rosnode = new ros::NodeHandle(this->robot_namespace);

    current_motor_state.mode = MotorStateMode::Position;
    current_motor_state.demultiply_value = GetValueFromElement<double>(sdf, "reduction_value", 1);
    current_motor_state.current_pos_rad = current_motor_state.goal_pos_rad = GetValueFromElement<double>(sdf, "default_pos", 0.0);
    current_motor_state.velocity_limit_rad_s = GetValueFromElement<double>(sdf, "default_vel_limit", 1);
    current_motor_state.torque_enabled = true;
    motor_allowed_error = GetValueFromElement<double>(sdf, "allowed_error", 0.01);
    current_motor_state.torque_limit = GetValueFromElement<double>(sdf, "default_torque_limit", 10);

    base_topic_name = GetValueFromElement<string>(sdf, "base_topic_name", "dynamixel_motor");

    joint->SetPosition(0, current_motor_state.current_pos_rad / current_motor_state.demultiply_value);

    auto mkTopicName = [&](string s) {
      return toString(robot_namespace, "/", base_topic_name, s);
    };

    command_subscriber = rosnode->subscribe<std_msgs::Float64>(
      mkTopicName("/command"), 10, [&] (const std_msgs::Float64::ConstPtr& msg) {
        current_motor_state.mode = MotorStateMode::Position;
        current_motor_state.goal_pos_rad = msg->data;
      }
    );

    vel_command = rosnode->subscribe<std_msgs::Float64>(
      mkTopicName("/vel_tor/command"), 10, [&] (const std_msgs::Float64::ConstPtr& msg) {
        current_motor_state.mode = MotorStateMode::Velocity;
        current_motor_state.velocity_rad_s = msg->data;
      }
    );


    dynamixel_joint_state_publisher = rosnode->advertise<MsgType>(
      mkTopicName("/state"), 10);

    InitServices();

    string motor_name = GetValueFromElement<string>(sdf, "motor_name", joint->GetName());

    // listen to the update event (broadcast every simulation iteration)
    update_connection =
      event::Events::ConnectWorldUpdateBegin(
          boost::bind(&GazeboRosDynamixelMotor::OnWorldUpdate, this));
  }


void GazeboRosDynamixelMotor::InitServices()
{
  auto mkServiceName = [&](string s) {
    return toString(robot_namespace, "/", base_topic_name, s);
  };

  set_speed_service = rosnode->advertiseService(mkServiceName("/set_speed"), &GazeboRosDynamixelMotor::SetSpeedService, this);

//   std::function< bool(dynamixel_controllers::SetSpeed::Request&, dynamixel_controllers::SetSpeed::Response& res) > a;
//   a = [&] (
//     dynamixel_controllers::SetSpeed::Request& req,
//     dynamixel_controllers::SetSpeed::Response& res
//   ) -> bool {
//     current_motor_state.velocity_rad_s = req.speed;
//     return true;
//   };
//
//   set_speed_service = rosnode->advertiseService(mkServiceName("/set_speed"), a);
//

  enable_torque_service = rosnode->advertiseService(mkServiceName("/torque_enable"),
        (boost::function<bool(dynamixel_controllers::TorqueEnable::Request&,
            dynamixel_controllers::TorqueEnable::Response&)>) ([&] (
    dynamixel_controllers::TorqueEnable::Request& req,
    dynamixel_controllers::TorqueEnable::Response& res
  ) {
    current_motor_state.torque_enabled = req.torque_enable;
    return true;
  }) );

  boost::function<bool(dynamixel_controllers::SetTorqueLimit::Request&,
      dynamixel_controllers::SetTorqueLimit::Response&)> torque_limit_f =  [&] (
    dynamixel_controllers::SetTorqueLimit::Request& req,
    dynamixel_controllers::SetTorqueLimit::Response& res
  ) {
    current_motor_state.torque_limit = req.torque_limit;
    return true;
  };

  set_torque_limit_service = rosnode->advertiseService(mkServiceName("/set_torque_limit"), torque_limit_f);
//
//   set_torque_limit_service = rosnode->advertiseService(mkServiceName("/set_torque"), [&] (
//     dynamixel_controllers::SetTorque::Request& req,
//     dynamixel_controllers::SetTorque::Response& res
//   ) {
//     ros_error("/set_torque not yet implemented");
//     return false;
//   });

  // TODO: add these
// self.compliance_slope_service = rospy.Service(self.controller_namespace + '/set_compliance_slope', SetComplianceSlope, self.process_set_compliance_slope)
// self.compliance_marigin_service = rospy.Service(self.controller_namespace + '/set_compliance_margin', SetComplianceMargin, self.process_set_compliance_margin)
// self.compliance_punch_service = rospy.Service(self.controller_namespace + '/set_compliance_punch', SetCompliancePunch, self.process_set_compliance_punch)
// self.torque_service = rospy.Service(self.controller_namespace + '/set_torque', SetTorque, self.process_set_torque)

}
bool GazeboRosDynamixelMotor::SetSpeedService(dynamixel_controllers::SetSpeed::Request& req, dynamixel_controllers::SetSpeed::Response& res)
{
  current_motor_state.velocity_rad_s = req.speed;
  return true;
}

  // Finalize the controller
  void GazeboRosDynamixelMotor::Shutdown() {
    alive_ = false;
    rosnode->shutdown();
  }

  dynamixel_msgs::JointState GazeboRosDynamixelMotor::createJointStateMsg(const std::string& name, const MotorState& motor_state)
  {
    dynamixel_msgs::JointState msg;
    msg.name = name;
    msg.motor_ids = std::vector<int>{ motor_state.motor_id };
    msg.motor_temps = std::vector<int>{ motor_state.motor_temp };
    msg.current_pos = motor_state.current_pos_rad;
    msg.goal_pos = motor_state.goal_pos_rad;
    msg.is_moving = motor_state.is_moving;
    msg.error = motor_state.error_rad;
    msg.velocity = motor_state.velocity_rad_s;
    msg.load = motor_state.load;

    return msg;
  }

  MotorState gazebo::GazeboRosDynamixelMotor::ReadMotor() const
  {
    MotorState read_motor_state = current_motor_state;
    double arm_angle_rad = joint->GetAngle(0).Radian();
    read_motor_state.current_pos_rad = arm_angle_rad * current_motor_state.demultiply_value;

    if(read_motor_state.mode == MotorStateMode::Position) {
      double pos_delta_rad = (read_motor_state.goal_pos_rad - read_motor_state.current_pos_rad);
      read_motor_state.error_rad = pos_delta_rad;
    } else {
      read_motor_state.error_rad = 0;
    }


    read_motor_state.is_moving = read_motor_state.velocity_rad_s != 0 && read_motor_state.torque_enabled;
    read_motor_state.load = joint->GetForceTorque(0).body2Torque.x; // TODO: the axis may be wrong, review this

    read_motor_state.motor_temp = nextGaussian<int>(24, 2);

    return read_motor_state;
  }


  void GazeboRosDynamixelMotor::UpdateMotor(const MotorState& read_motor_state)
  {

    if(read_motor_state.mode == MotorStateMode::Position) {
      double pos_delta_rad = (read_motor_state.goal_pos_rad - read_motor_state.current_pos_rad);
      bool goal_reached =  fabs(pos_delta_rad) < motor_allowed_error;
      if(!goal_reached) {
        double target_velocity = 0;
        target_velocity = Td::sgn(pos_delta_rad) * read_motor_state.velocity_limit_rad_s * Td::sgn(current_motor_state.demultiply_value);
        joint->SetParam("vel", 0, target_velocity);
      } else {
        joint->SetParam("vel", 0, 0.0);
      }
    }

    if(read_motor_state.torque_enabled) {
      joint->SetParam("fmax", 0, read_motor_state.torque_limit);
    } else {
      joint->SetParam("fmax", 0, 0.0);
    }
  }

  void GazeboRosDynamixelMotor::OnWorldUpdate()
  {
    current_motor_state = ReadMotor();

    MsgType joint_state_msg = createJointStateMsg(motor_name, current_motor_state);
    dynamixel_joint_state_publisher.publish<MsgType>(joint_state_msg);

    UpdateMotor(current_motor_state);
  }

  GZ_REGISTER_MODEL_PLUGIN(GazeboRosDynamixelMotor)
}
