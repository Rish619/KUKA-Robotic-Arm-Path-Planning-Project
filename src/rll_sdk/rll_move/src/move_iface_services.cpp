/*
 * This file is part of the Robot Learning Lab SDK
 *
 * Copyright (C) 2018-2019 Wolfgang Wiedmeyer <wolfgang.wiedmeyer@kit.edu>
 * Copyright (C) 2019 Mark Weinreuter <uieai@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <moveit/trajectory_processing/iterative_time_parameterization.h>
#include <rll_move/move_iface_services.h>

const std::string RLLMoveIfaceServices::ROBOT_READY_SRV_NAME = "robot_ready";
const std::string RLLMoveIfaceServices::MOVE_PTP_SRV_NAME = "move_ptp";
const std::string RLLMoveIfaceServices::MOVE_PTP_ARMANGLE_SRV_NAME = "move_ptp_armangle";
const std::string RLLMoveIfaceServices::MOVE_LIN_SRV_NAME = "move_lin";
const std::string RLLMoveIfaceServices::MOVE_LIN_ARMANGLE_SRV_NAME = "move_lin_armangle";
const std::string RLLMoveIfaceServices::MOVE_JOINTS_SRV_NAME = "move_joints";
const std::string RLLMoveIfaceServices::MOVE_RANDOM_SRV_NAME = "move_random";
const std::string RLLMoveIfaceServices::GET_POSE_SRV_NAME = "get_current_pose";
const std::string RLLMoveIfaceServices::GET_JOINT_VALUES_SRV_NAME = "get_current_joint_values";

RLLMoveIfaceServices::RLLMoveIfaceServices()
{
  setupPermissions();
}

void RLLMoveIfaceServices::setupPermissions()
{
  only_during_job_run_permission_ = permissions_.registerPermission("only_during_job_run", false);
  move_permission_ = permissions_.registerPermission("allowed_to_move", false);

  // by default every service requires the move permission unless explicitly changed
  auto default_permissions = only_during_job_run_permission_ | move_permission_;
  permissions_.setDefaultRequiredPermissions(default_permissions);

  // robot ready service check is always allowed
  permissions_.setRequiredPermissionsFor(RLLMoveIfaceServices::ROBOT_READY_SRV_NAME,
                                         Permissions::NO_PERMISSION_REQUIRED);
}

bool RLLMoveIfaceServices::robotReadySrv(std_srvs::Trigger::Request& /*req*/, std_srvs::Trigger::Response& resp)
{
  RLLErrorCode error_code = beforeServiceCall(RLLMoveIfaceServices::ROBOT_READY_SRV_NAME);
  if (error_code.succeeded())
  {
    error_code = resetToHome();
  }

  error_code = afterServiceCall(RLLMoveIfaceServices::ROBOT_READY_SRV_NAME, error_code);
  resp.success = error_code.succeededSrv();
  return true;
}

void RLLMoveIfaceServices::abortDueToCriticalFailure()
{
  iface_state_.enterErrorState();
}

void RLLMoveIfaceServices::handleFailureSeverity(const RLLErrorCode& error_code)
{
  // check if a error condition matches, if not assume critical failure
  if (error_code.isInvalidInput())
  {
    ROS_WARN("A failure due to invalid input occurred. error: %s", error_code.message());
  }
  else if (error_code.isRecoverableFailure())
  {
    ROS_WARN("A recoverable failure occurred, further operations are still possible. error: %s", error_code.message());
  }
  else
  {
    ROS_FATAL("A critical failure occurred! All further operations will be cancelled. error: %s", error_code.message());

    abortDueToCriticalFailure();
  }
}

RLLErrorCode RLLMoveIfaceServices::beforeServiceCall(const std::string& srv_name)
{
  ROS_DEBUG("service '%s' requested", srv_name.c_str());

  bool only_during_job_run = permissions_.isPermissionRequiredFor(srv_name, only_during_job_run_permission_);
  RLLErrorCode error_code = iface_state_.beginServiceCall(srv_name, only_during_job_run);
  if (error_code.failed())
  {
    return error_code;
  }

  // check if this service call is permitted
  if (!permissions_.areAllRequiredPermissionsSetFor(srv_name))
  {
    return RLLErrorCode::INSUFFICIENT_PERMISSION;
  }

  if (!manipCurrentStateAvailable())
  {
    return RLLErrorCode::MANIPULATOR_NOT_AVAILABLE;
  }

  return RLLErrorCode::SUCCESS;
}

RLLErrorCode RLLMoveIfaceServices::afterServiceCall(const std::string& srv_name,
                                                    const RLLErrorCode& previous_error_code)
{
  bool only_during_job_run = permissions_.isPermissionRequiredFor(srv_name, only_during_job_run_permission_);
  RLLErrorCode error_code = iface_state_.endServiceCall(srv_name, only_during_job_run);
  ROS_DEBUG("service '%s' ended", srv_name.c_str());

  // a previous error code is probably more specific and takes precedence
  error_code = previous_error_code.determineWorse(error_code);

  if (error_code.failed())
  {
    ROS_WARN("'%s' service call failed!", srv_name.c_str());
    handleFailureSeverity(error_code);
  }

  return error_code;
}

bool RLLMoveIfaceServices::moveRandomSrv(rll_msgs::MoveRandom::Request& req, rll_msgs::MoveRandom::Response& resp)
{
  return controlledMovementExecution(req, &resp, MOVE_RANDOM_SRV_NAME, &RLLMoveIfaceServices::moveRandom);
}

RLLErrorCode RLLMoveIfaceServices::moveRandom(const rll_msgs::MoveRandom::Request& /*req*/,
                                              rll_msgs::MoveRandom::Response* resp)
{
  bool success = false;
  int retry_counter = 0;
  geometry_msgs::Pose random_pose;
  std::vector<double> goal_joint_values(RLL_NUM_JOINTS);

  while (!success && retry_counter < 30)
  {
    retry_counter++;

    random_pose = manip_move_group_.getRandomPose().pose;
    if (poseGoalTooClose(random_pose))
    {
      success = false;
      ROS_INFO("last random pose too close to start pose, retrying...");
      continue;
    }
    RLLErrorCode error_code = poseGoalInCollision(random_pose, &goal_joint_values);
    if (error_code.failed())
    {
      success = false;
      ROS_INFO("last random pose is in collision, retrying...");
      continue;
    }

    manip_move_group_.setJointValueTarget(goal_joint_values);

    error_code = runPTPTrajectory(&manip_move_group_);
    // make sure nothing major went wrong. only repeat in case of non critical errors
    if (error_code.isCriticalFailure())
    {
      return error_code;
    }

    success = error_code.succeeded();
    if (!success)
    {
      ROS_INFO("planning failed for last random pose, retrying...");
    }
  }

  if (success)
  {
    ROS_INFO("moved to random position");
    resp->pose = random_pose;
  }
  else
  {
    ROS_WARN("failed to move to random position");
    return RLLErrorCode::NO_RANDOM_POSITION_FOUND;
  }

  return RLLErrorCode::SUCCESS;
}

bool RLLMoveIfaceServices::moveLinSrv(rll_msgs::MoveLin::Request& req, rll_msgs::MoveLin::Response& resp)
{
  return controlledMovementExecution(req, &resp, MOVE_LIN_SRV_NAME, &RLLMoveIfaceServices::moveLin);
}

RLLErrorCode RLLMoveIfaceServices::moveLin(const rll_msgs::MoveLin::Request& req, rll_msgs::MoveLin::Response* /*resp*/)
{
  return moveToGoalLinear(req.pose);
}

bool RLLMoveIfaceServices::moveLinArmangleSrv(rll_msgs::MoveLinArmangle::Request& req,
                                              rll_msgs::MoveLinArmangle::Response& resp)
{
  return controlledMovementExecution(req, &resp, MOVE_LIN_ARMANGLE_SRV_NAME, &RLLMoveIfaceServices::moveLinArmangle);
}

RLLErrorCode RLLMoveIfaceServices::moveLinArmangle(const rll_msgs::MoveLinArmangle::Request& req,
                                                   rll_msgs::MoveLinArmangle::Response* /*resp*/)
{
  double arm_angle_goal = req.arm_angle;
  auto dir = static_cast<int>(req.direction);

  if (!armangleInRange(arm_angle_goal))
  {
    return RLLErrorCode::INVALID_INPUT;
  }

  std::vector<double> seed = manip_move_group_.getCurrentJointValues();

  // get arm angle in start pose
  geometry_msgs::Pose pose_tmp;
  double arm_angle_start;
  int config;
  kinematics_plugin_->getPositionFK(seed, &pose_tmp, &arm_angle_start, &config);

  // calculate waypoints
  std::vector<geometry_msgs::Pose> waypoints_pose;
  std::vector<double> arm_angles;
  size_t steps_arm_angle = numStepsArmAngle(arm_angle_start, arm_angle_goal);
  RLLErrorCode error_code =
      interpolatePosesLinear(manip_move_group_.getCurrentPose().pose, req.pose, &waypoints_pose, steps_arm_angle);
  if (error_code.failed())
  {
    return error_code;
  }

  for (auto& waypoint : waypoints_pose)
  {
    transformPoseForIK(&waypoint);
  }
  interpolateArmangleLinear(arm_angle_start, arm_angle_goal, dir, waypoints_pose.size(), &arm_angles);
  std::vector<robot_state::RobotStatePtr> path;
  error_code = computeLinearPathArmangle(waypoints_pose, arm_angles, seed, &path);
  if (error_code.failed())
  {
    return error_code;
  }

  robot_trajectory::RobotTrajectory rt(manip_model_, manip_move_group_.getName());
  for (const auto& path_pose : path)
  {
    rt.addSuffixWayPoint(path_pose, 0.0);
  }
  moveit_msgs::RobotTrajectory trajectory;
  rt.getRobotTrajectoryMsg(trajectory);

  // check for collisions
  if (!planning_scene_->isPathValid(rt))
  {  // TODO(updim): maybe output collision state
    ROS_ERROR("There is a collision along the path");
    return RLLErrorCode::ONLY_PARTIAL_PATH_PLANNED;
  }

  // moveLinArmangle service calls are disallowed to use cartesian_time_parametrization
  return runLinearTrajectory(trajectory, false);
}

bool RLLMoveIfaceServices::movePTPSrv(rll_msgs::MovePTP::Request& req, rll_msgs::MovePTP::Response& resp)
{
  return controlledMovementExecution(req, &resp, MOVE_PTP_SRV_NAME, &RLLMoveIfaceServices::movePTP);
}

RLLErrorCode RLLMoveIfaceServices::movePTP(const rll_msgs::MovePTP::Request& req, rll_msgs::MovePTP::Response* /*resp*/)
{
  std::vector<double> goal_joint_values(RLL_NUM_JOINTS);

  manip_move_group_.setStartStateToCurrentState();

  RLLErrorCode error_code = poseGoalInCollision(req.pose, &goal_joint_values);
  if (error_code.failed())
  {
    return error_code;
  }

  manip_move_group_.setJointValueTarget(goal_joint_values);

  return runPTPTrajectory(&manip_move_group_);
}

bool RLLMoveIfaceServices::movePTPArmangleSrv(rll_msgs::MovePTPArmangle::Request& req,
                                              rll_msgs::MovePTPArmangle::Response& resp)
{
  return controlledMovementExecution(req, &resp, MOVE_PTP_ARMANGLE_SRV_NAME, &RLLMoveIfaceServices::movePTPArmangle);
}

RLLErrorCode RLLMoveIfaceServices::movePTPArmangle(const rll_msgs::MovePTPArmangle::Request& req,
                                                   rll_msgs::MovePTPArmangle::Response* /*resp*/)
{
  bool success;
  moveit_msgs::MoveItErrorCodes error_code;
  std::vector<double> seed;
  std::vector<double> sol;
  double arm_angle = req.arm_angle;

  if (!armangleInRange(arm_angle))
  {
    return RLLErrorCode::INVALID_INPUT;
  }

  // get solution
  seed = manip_move_group_.getCurrentJointValues();
  geometry_msgs::Pose pose_tip = req.pose;
  transformPoseForIK(&pose_tip);
  if (!kinematics_plugin_->getPositionIKarmangle(pose_tip, seed, &sol, &error_code, arm_angle))
  {
    ROS_ERROR("Inverse kinematics calculation failed");
    return RLLErrorCode::INVALID_TARGET_POSE;
  }

  manip_move_group_.setStartStateToCurrentState();
  success = manip_move_group_.setJointValueTarget(sol);

  if (!success)
  {
    ROS_ERROR("requested joint values are out of range");
    return RLLErrorCode::JOINT_VALUES_OUT_OF_RANGE;
  }

  return runPTPTrajectory(&manip_move_group_);
}

bool RLLMoveIfaceServices::moveJointsSrv(rll_msgs::MoveJoints::Request& req, rll_msgs::MoveJoints::Response& resp)
{
  return controlledMovementExecution(req, &resp, MOVE_JOINTS_SRV_NAME, &RLLMoveIfaceServices::moveJoints);
}

RLLErrorCode RLLMoveIfaceServices::moveJoints(const rll_msgs::MoveJoints::Request& req,
                                              rll_msgs::MoveJoints::Response* /*resp*/)
{
  bool success;
  std::vector<double> joints(RLL_NUM_JOINTS);
  moveit::planning_interface::MoveGroupInterface::Plan my_plan;

  joints[0] = req.joint_1;
  joints[1] = req.joint_2;
  joints[2] = req.joint_3;
  joints[3] = req.joint_4;
  joints[4] = req.joint_5;
  joints[5] = req.joint_6;
  joints[6] = req.joint_7;

  if (jointsGoalInCollision(joints))
  {
    return RLLErrorCode::GOAL_IN_COLLISION;
  }

  manip_move_group_.setStartStateToCurrentState();
  success = manip_move_group_.setJointValueTarget(joints);
  if (!success)
  {
    ROS_ERROR("requested joint values are out of range");
    return RLLErrorCode::JOINT_VALUES_OUT_OF_RANGE;
  }

  return runPTPTrajectory(&manip_move_group_);
}

bool RLLMoveIfaceServices::getCurrentJointValuesSrv(rll_msgs::GetJointValues::Request& /*req*/,
                                                    rll_msgs::GetJointValues::Response& resp)
{
  RLLErrorCode error_code = beforeServiceCall(RLLMoveIfaceServices::GET_JOINT_VALUES_SRV_NAME);

  if (error_code.succeeded())
  {
    std::vector<double> joints = manip_move_group_.getCurrentJointValues();
    resp.joint_1 = joints[0];
    resp.joint_2 = joints[1];
    resp.joint_3 = joints[2];
    resp.joint_4 = joints[3];
    resp.joint_5 = joints[4];
    resp.joint_6 = joints[5];
    resp.joint_7 = joints[6];
  }

  error_code = afterServiceCall(RLLMoveIfaceServices::GET_JOINT_VALUES_SRV_NAME, error_code);
  resp.error_code = error_code.value();
  resp.success = error_code.succeededSrv();
  return true;
}

bool RLLMoveIfaceServices::getCurrentPoseSrv(rll_msgs::GetPose::Request& /*req*/, rll_msgs::GetPose::Response& resp)
{
  geometry_msgs::Pose pose_tmp;
  double arm_angle;
  int config;

  RLLErrorCode error_code = beforeServiceCall(RLLMoveIfaceServices::GET_POSE_SRV_NAME);

  if (error_code.succeeded())
  {
    std::vector<double> joints = manip_move_group_.getCurrentJointValues();
    resp.pose = manip_move_group_.getCurrentPose().pose;
    kinematics_plugin_->getPositionFK(joints, &pose_tmp, &arm_angle, &config);
    resp.arm_angle = arm_angle;
    resp.config = config;
  }

  error_code = afterServiceCall(RLLMoveIfaceServices::GET_POSE_SRV_NAME, error_code);
  resp.error_code = error_code.value();
  resp.success = error_code.succeededSrv();
  return true;
}

RLLErrorCode RLLMoveIfaceServices::resetToHome()
{
  if (!manipCurrentStateAvailable())
  {
    return RLLErrorCode::MANIPULATOR_NOT_AVAILABLE;
  }

  std::vector<double> start = manip_move_group_.getCurrentJointValues();
  std::vector<double> goal = getJointValuesFromNamedTarget(HOME_TARGET_NAME);

  if (jointsGoalTooClose(start, goal))
  {
    // this is acceptable since we want to move here anyways -> skip movement
    ROS_INFO("resetToHome: no movement required, already at target position");
  }
  else
  {
    manip_move_group_.setStartStateToCurrentState();
    manip_move_group_.setNamedTarget(HOME_TARGET_NAME);

    RLLErrorCode error_code = runPTPTrajectory(&manip_move_group_);
    if (error_code.failed())
    {
      return error_code;
    }
  }

  return RLLErrorCode::SUCCESS;
}

/*
 * Local Variables:
 * c-file-style: "google"
 * End:
 */
