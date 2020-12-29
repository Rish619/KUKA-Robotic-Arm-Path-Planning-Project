/*
 * This file is part of the Robot Learning Lab SDK
 *
 * Copyright (C) 2018-2020 Wolfgang Wiedmeyer <wolfgang.wiedmeyer@kit.edu>
 * Copyright (C) 2019 Mark Weinreuter <mark.weinreuter@kit.edu>
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

#include <eigen_conversions/eigen_msg.h>
#include <tf2_ros/transform_listener.h>

#include <rll_move/move_iface_planning.h>

const double RLLMoveIfacePlanning::DEFAULT_VELOCITY_SCALING_FACTOR = 0.4;
const double RLLMoveIfacePlanning::DEFAULT_ACCELERATION_SCALING_FACTOR = 0.4;

const double RLLMoveIfacePlanning::DEFAULT_LINEAR_EEF_STEP = 0.001;
const double RLLMoveIfacePlanning::DEFAULT_ROTATION_EEF_STEP = 1 * M_PI / 180;
// TODO(wolfgang): get rid of the jump threshold. Instead, modify the IK error code with a flag to report if new arm
// angle is in same interval as old arm angle -> no jump.
// The jump threshold is too restrictive and throws errors on otherwise admissible trajectories.
const double RLLMoveIfacePlanning::DEFAULT_LINEAR_JUMP_THRESHOLD = 10;
const size_t RLLMoveIfacePlanning::LINEAR_MIN_STEPS_FOR_JUMP_THRESH = 10;

const std::string RLLMoveIfacePlanning::MANIP_PLANNING_GROUP = "manipulator";

const std::string RLLMoveIfacePlanning::HOME_TARGET_NAME = "home_bow";

RLLMoveIfacePlanning::RLLMoveIfacePlanning() : manip_move_group_(MANIP_PLANNING_GROUP)
{
  ns_ = ros::this_node::getNamespace();
// remove the slashes at the beginning
#if ROS_VERSION_MINIMUM(1, 14, 3)  // Melodic
  ns_.erase(0, 1);
#else  // Kinetic and older
  ns_.erase(0, 2);
#endif
  ROS_INFO("starting in ns %s", ns_.c_str());

  node_name_ = ros::this_node::getName();

  ros::param::get("move_group/trajectory_execution/allowed_start_tolerance", allowed_start_tolerance_);

  ros::param::get("~eef_type", eef_type_);
  if (eef_type_.empty())
  {
    ROS_ERROR("No EEF type specified, please pass a eef_type parameter, using default egl90");
    eef_type_ = "egl90";
  }
  ROS_INFO("Using EEF type: %s", eef_type_.c_str());

  // for now we only support two gripper types
  no_gripper_attached_ = !(eef_type_ == "egl90" || eef_type_ == "crg200");
  if (no_gripper_attached_)
  {
    ROS_INFO("Configured to not use a gripper");
  }

  manip_move_group_.setPlannerId("RRTConnectkConfigDefault");
  manip_move_group_.setPlanningTime(2.0);
  manip_move_group_.setPoseReferenceFrame("world");

  manip_move_group_.setMaxVelocityScalingFactor(DEFAULT_VELOCITY_SCALING_FACTOR);
  manip_move_group_.setMaxAccelerationScalingFactor(DEFAULT_ACCELERATION_SCALING_FACTOR);

  manip_model_ = manip_move_group_.getRobotModel();
  manip_joint_model_group_ = manip_model_->getJointModelGroup(manip_move_group_.getName());

  // each configurable EEF will have this link
  std::string ee_link = ns_ + "_link_tcp";
  manip_move_group_.setEndEffectorLink(ee_link);

  planning_scene_monitor_ = std::make_shared<planning_scene_monitor::PlanningSceneMonitor>("robot_description");

  planning_scene_monitor_->requestPlanningSceneState("get_planning_scene");
  planning_scene_ = planning_scene_monitor::LockedPlanningSceneRO(planning_scene_monitor_);
  acm_ = planning_scene_->getAllowedCollisionMatrix();  // this is only a copy of the ACM from the current planning
                                                        // scene, it is not updated?

  // startup checks, shutdown the node if something is wrong
  if (isInitialStateInCollision() || !isCollisionLinkAvailable() || !getKinematicsSolver() || !initConstTransforms())
  {
    ROS_FATAL("Startup checks failed, shutting the node down!");
    ros::shutdown();
  }
}

const std::string& RLLMoveIfacePlanning::getNamespace()
{
  return ns_;
}

const std::string& RLLMoveIfacePlanning::getEEFType()
{
  return eef_type_;
}

bool RLLMoveIfacePlanning::isInitialStateInCollision()
{
  // If we start in a colliding state, it is often not apparant why the robot isn't moving
  robot_state::RobotState current_state = getCurrentRobotState();
  if (stateInCollision(&current_state))
  {
    ROS_FATAL("Starting state is in collision! Please verify your setup!");
    return true;
  }

  return false;
}

bool RLLMoveIfacePlanning::isCollisionLinkAvailable()
{
  std::string collision_link;
  bool success = ros::param::get(node_name_ + "/collision_link", collision_link);
  if (!success)
  {
    ROS_FATAL("No 'collision_link' param set. Please specify a collision_link param "
              "to verify that the collision model is loaded.");
    return false;
  }

  tf2_ros::Buffer tf_buffer;
  // required to allow specifying a timeout in lookupTransform
  tf2_ros::TransformListener listener(tf_buffer);

  // if the workcell is loaded correctly the collision_link should be available
  success = tf_buffer.canTransform("world", collision_link, ros::Time(0), ros::Duration(5));

  if (!success)
  {
    ROS_FATAL("Failed to look up the collision link '%s'. Did you launch the correct file?", collision_link.c_str());
    return false;
  }

  ROS_DEBUG("collision link '%s' lookup succeeded", collision_link.c_str());
  return true;
}

bool RLLMoveIfacePlanning::manipCurrentStateAvailable()
{
  // Sometimes, the current state cannot be retrieved and a NULL pointer exception is thrown
  // somewhere. Check here if the current state can be retrieved. Other methods can use this
  // function to abort further MoveIt commands and to avoid sigterms.

  robot_state::RobotStatePtr current_state = manip_move_group_.getCurrentState();
  if (current_state == nullptr)
  {
    ROS_FATAL("Current robot state cannot be retrieved.");
    return false;
  }

  return true;
}

RLLErrorCode RLLMoveIfacePlanning::runPTPTrajectory(moveit::planning_interface::MoveGroupInterface* move_group,
                                                    bool for_gripper)
{
  moveit::planning_interface::MoveGroupInterface::Plan my_plan;
  moveit::planning_interface::MoveItErrorCode moveit_error_code;
  bool success;

  moveit_error_code = move_group->plan(my_plan);
  RLLErrorCode error_code = convertMoveItErrorCode(moveit_error_code);
  if (error_code.failed())
  {
    ROS_WARN("MoveIt planning failed: error code %s", stringifyMoveItErrorCodes(moveit_error_code));
    return error_code;
  }

  if (!for_gripper)
  {
    error_code = checkTrajectory(my_plan.trajectory_);
    if (error_code.failed())
    {
      return error_code;
    }

    success = modifyPtpTrajectory(&my_plan.trajectory_);
    if (!success)
    {
      return RLLErrorCode::TRAJECTORY_MODIFICATION_FAILED;
    }
  }

  return execute(move_group, my_plan);
}

RLLErrorCode RLLMoveIfacePlanning::execute(moveit::planning_interface::MoveGroupInterface* move_group,
                                           const moveit::planning_interface::MoveGroupInterface::Plan& plan)
{
  moveit::planning_interface::MoveItErrorCode moveit_error_code;

  moveit_error_code = move_group->execute(plan);
  RLLErrorCode error_code = convertMoveItErrorCode(moveit_error_code);
  if (error_code.failed())
  {
    ROS_WARN("MoveIt plan execution failed: error code %s", stringifyMoveItErrorCodes(moveit_error_code));
    return error_code;
  }

  if (move_group->getName() != MANIP_PLANNING_GROUP)  // run only for manipulator
  {
    ros::Duration(.25).sleep();  // wait a bit just in case the gripper is still moving
    return RLLErrorCode::SUCCESS;
  }

  std::vector<double> last_point = plan.trajectory_.joint_trajectory.points.back().positions;
  bool identical = false;
  std::vector<double> current_point;
  ros::Rate r(200);
  ros::Duration timeout(2);
  ros::Time begin = ros::Time::now();
  while (!identical && ros::Time::now() - begin < timeout)
  {
    // current state in move_group is not uptodate with last state from planning scene, so fetch directly from
    // planning scene
    getCurrentRobotState().copyJointGroupPositions(manip_joint_model_group_, current_point);
    for (size_t i = 0; i < last_point.size(); ++i)
    {
      if (fabs(current_point[i] - last_point[i]) >= allowed_start_tolerance_)
      {
        r.sleep();
        break;
      }
      if (i == last_point.size() - 1)
      {
        identical = true;
      }
    }
  }

  if (!identical)
  {
    ROS_FATAL("desired goal state was not reached");
    return RLLErrorCode::EXECUTION_FAILED;
  }

  return RLLErrorCode::SUCCESS;
}

geometry_msgs::Pose RLLMoveIfacePlanning::getCurrentPoseFromPlanningScene()
{
  getCurrentRobotState(true).update();  // TODO(mark): does this make a difference?
  return manip_move_group_.getCurrentPose().pose;
}

double RLLMoveIfacePlanning::distanceToCurrentPosition(const geometry_msgs::Pose& pose)
{
  geometry_msgs::Pose current_pose = getCurrentPoseFromPlanningScene();

  double distance =
      sqrt(pow(current_pose.position.x - pose.position.x, 2) + pow(current_pose.position.y - pose.position.y, 2) +
           pow(current_pose.position.z - pose.position.z, 2));

  ROS_INFO("Distance between current and goal: %.3f", distance);
  ROS_INFO_POS(" current", current_pose.position);
  ROS_INFO_POS("  target", pose.position);
  return distance;
}

bool RLLMoveIfacePlanning::tooCloseForLinearMovement(const geometry_msgs::Pose& goal)
{
  const double MIN_LIN_MOVEMENT_DISTANCE = 0.005;
  float distance = distanceToCurrentPosition(goal);
  return distance < MIN_LIN_MOVEMENT_DISTANCE;
}

RLLErrorCode RLLMoveIfacePlanning::moveToGoalLinear(const geometry_msgs::Pose& goal,
                                                    bool cartesian_time_parametrization)
{
  moveit_msgs::RobotTrajectory trajectory;
  std::vector<double> goal_joint_values(RLL_NUM_JOINTS);

  RLLErrorCode error_code = poseGoalInCollision(goal, &goal_joint_values);
  if (error_code.failed())
  {
    return error_code;
  }

  manip_move_group_.setStartStateToCurrentState();
  error_code = computeLinearPath(goal, &trajectory);
  if (error_code.failed())
  {
    return error_code;
  }

  return runLinearTrajectory(trajectory, cartesian_time_parametrization);
}

RLLErrorCode RLLMoveIfacePlanning::computeLinearPath(const geometry_msgs::Pose& goal,
                                                     moveit_msgs::RobotTrajectory* trajectory)
{
  return computeLinearPath(manip_move_group_.getCurrentJointValues(), goal, trajectory);
}

RLLErrorCode RLLMoveIfacePlanning::computeLinearPath(const std::vector<double>& start, const geometry_msgs::Pose& goal,
                                                     moveit_msgs::RobotTrajectory* trajectory)
{
  std::vector<geometry_msgs::Pose> waypoints_pose;

  geometry_msgs::Pose start_pose;
  double arm_angle;
  int config;
  kinematics_plugin_->getPositionFK(start, &start_pose, &arm_angle, &config);
  transformPoseFromFK(&start_pose);
  RLLErrorCode error_code = interpolatePosesLinear(start_pose, goal, &waypoints_pose);
  if (error_code.failed())
  {
    return error_code;
  }

  for (auto& waypoint : waypoints_pose)
  {
    transformPoseForIK(&waypoint);
  }

  double achieved = 0.0;
  std::vector<robot_state::RobotStatePtr> path;
  getPathIK(waypoints_pose, start, &path, &achieved);

  moveit::core::JumpThreshold thresh(DEFAULT_LINEAR_JUMP_THRESHOLD);
  achieved *= robot_state::RobotState::testJointSpaceJump(manip_joint_model_group_, path, thresh);

  if (achieved > 0.0 && achieved < 1.0)
  {
    ROS_ERROR("only achieved to compute %f of the requested path", achieved);
    return RLLErrorCode::ONLY_PARTIAL_PATH_PLANNED;
  }

  if (achieved <= 0.0)
  {
    ROS_ERROR("path planning completely failed");
    return RLLErrorCode::MOVEIT_PLANNING_FAILED;
  }

  robot_trajectory::RobotTrajectory rt(manip_model_, manip_move_group_.getName());
  for (const auto& path_pose : path)
  {
    rt.addSuffixWayPoint(path_pose, 0.0);
  }

  rt.getRobotTrajectoryMsg(*trajectory);

  if (trajectory->joint_trajectory.points.size() < LINEAR_MIN_STEPS_FOR_JUMP_THRESH)
  {
    ROS_ERROR("trajectory has not enough points to check for continuity, only got %lu",
              trajectory->joint_trajectory.points.size());
    return RLLErrorCode::TOO_FEW_WAYPOINTS;
  }

  // check for collisions
  if (!planning_scene_->isPathValid(rt))
  {  // TODO(updim): maybe output collision state
    ROS_ERROR("There is a collision along the path");
    return RLLErrorCode::ONLY_PARTIAL_PATH_PLANNED;
  }

  return RLLErrorCode::SUCCESS;
}

RLLErrorCode RLLMoveIfacePlanning::runLinearTrajectory(const moveit_msgs::RobotTrajectory& trajectory,
                                                       bool cartesian_time_parametrization)
{
  moveit::planning_interface::MoveGroupInterface::Plan my_plan;
  bool success;

  my_plan.trajectory_ = trajectory;

  RLLErrorCode error_code = checkTrajectory(my_plan.trajectory_);
  if (error_code.failed())
  {
    return error_code;
  }

  // time parametrization happens in joint space by default
  if (cartesian_time_parametrization)
  {
    success = modifyLinTrajectory(&my_plan.trajectory_);
    if (!success)
    {
      return RLLErrorCode::TRAJECTORY_MODIFICATION_FAILED;
    }
  }
  else
  {
    success = modifyPtpTrajectory(&my_plan.trajectory_);
    if (!success)
    {
      return RLLErrorCode::TRAJECTORY_MODIFICATION_FAILED;
    }
  }

  ROS_INFO_STREAM("trajectory duration is "
                  << my_plan.trajectory_.joint_trajectory.points.back().time_from_start.toSec() << " seconds");

  return execute(&manip_move_group_, my_plan);
}

RLLErrorCode RLLMoveIfacePlanning::checkTrajectory(const moveit_msgs::RobotTrajectory& trajectory)
{
  if (trajectory.joint_trajectory.points.size() < 3)
  {
    ROS_WARN("trajectory has less than 3 points");
    return RLLErrorCode::TOO_FEW_WAYPOINTS;
  }

  std::vector<double> start = trajectory.joint_trajectory.points[0].positions;
  std::vector<double> goal = trajectory.joint_trajectory.points.back().positions;

  if (jointsGoalTooClose(start, goal))
  {
    ROS_WARN("trajectory: start state too close to goal state");
    return RLLErrorCode::GOAL_TOO_CLOSE_TO_START;
  }
  if (jointsGoalInCollision(goal))
  {
    return RLLErrorCode::GOAL_IN_COLLISION;
  }

  return RLLErrorCode::SUCCESS;
}

std::vector<double> RLLMoveIfacePlanning::getJointValuesFromNamedTarget(const std::string& name)
{
  std::vector<double> goal;
  std::map<std::string, double> home_pose = manip_move_group_.getNamedTargetValues(name);

  goal.reserve(RLL_NUM_JOINTS);
  for (const auto& it : home_pose)
  {
    goal.push_back(it.second);
  }
  return goal;
}

bool RLLMoveIfacePlanning::jointsGoalTooClose(const std::vector<double>& start, const std::vector<double>& goal)
{
  const double MIN_DISTANCE = 0.02;  // a little more than 1 degree

  double distance = 0.0;
  for (unsigned int i = 0; i < start.size(); ++i)
  {
    distance += fabs(start[i] - goal[i]);
  }

  return (distance < MIN_DISTANCE);
}

bool RLLMoveIfacePlanning::poseGoalTooClose(const geometry_msgs::Pose& goal)
{
  std::vector<double> goal_joints;
  moveit_msgs::MoveItErrorCodes error_code;
  std::vector<double> seed = manip_move_group_.getCurrentJointValues();

  geometry_msgs::Pose pose_tip = goal;
  transformPoseForIK(&pose_tip);
  if (!kinematics_plugin_->searchPositionIK(pose_tip, seed, 0.1, goal_joints, error_code))
  {
    ROS_WARN("goal pose for goal distance check invalid: error code %s", stringifyMoveItErrorCodes(error_code));
    return true;
  }

  if (jointsGoalTooClose(seed, goal_joints))
  {
    ROS_WARN("goal joint values too close to start joint values");
    return true;
  }

  // in case we chose different joint values check the cartesian distance too
  // TODO(mark): consider difference in orientation as well
  return distanceToCurrentPosition(goal) <= 0.001;
}

bool RLLMoveIfacePlanning::jointsGoalInCollision(const std::vector<double>& goal)
{
  robot_state::RobotState goal_state = getCurrentRobotState();
  goal_state.setJointGroupPositions(manip_joint_model_group_, goal);
  if (stateInCollision(&goal_state))
  {
    ROS_WARN("robot would be in collision for goal pose");
    return true;
  }

  return false;
}

RLLErrorCode RLLMoveIfacePlanning::poseGoalInCollision(const geometry_msgs::Pose& goal)
{
  std::vector<double> joints(7);
  return poseGoalInCollision(goal, &joints);
}

RLLErrorCode RLLMoveIfacePlanning::poseGoalInCollision(const geometry_msgs::Pose& goal,
                                                       std::vector<double>* goal_joint_values)
{
  RLLKinSeedState ik_seed_state;
  RLLInvKinOptions ik_options;
  RLLKinSolutions ik_solutions;
  std::vector<double> current_joint_values(RLL_NUM_JOINTS);

  robot_state::RobotState current_state = getCurrentRobotState();
  current_state.copyJointGroupPositions(manip_joint_model_group_, current_joint_values);

  ik_options.global_configuration_mode = RLLInvKinOptions::RETURN_ALL_GLOBAL_CONFIGS;
  geometry_msgs::Pose goal_ik = goal;
  transformPoseForIK(&goal_ik);
  ik_seed_state.emplace_back(current_joint_values);
  ik_seed_state.emplace_back(current_joint_values);
  RLLKinMsg result = kinematics_plugin_->callRLLIK(goal_ik, ik_seed_state, &ik_solutions, ik_options);
  if (result.error())
  {
    ROS_WARN_STREAM("no IK solution found for given goal pose: " << result.message());
    return RLLErrorCode::NO_IK_SOLUTION_FOUND;
  }

  robot_state::RobotState goal_state = current_state;
  for (size_t i = 0; i < ik_solutions.size(); ++i)
  {
    ik_solutions[i].getJoints(goal_joint_values);
    goal_state.setJointGroupPositions(manip_joint_model_group_, *goal_joint_values);
    if (!stateInCollision(&goal_state))
    {
      return RLLErrorCode::SUCCESS;
    }
  }

  ROS_WARN("robot would be in collision for given goal pose");
  return RLLErrorCode::GOAL_IN_COLLISION;
}

robot_state::RobotState RLLMoveIfacePlanning::getCurrentRobotState(bool wait_for_state)
{
  if (wait_for_state)
  {
    planning_scene_monitor_->waitForCurrentRobotState(ros::Time::now());
  }

  planning_scene_monitor_->requestPlanningSceneState("get_planning_scene");
  planning_scene_monitor::LockedPlanningSceneRW planning_scene_rw(planning_scene_monitor_);
  planning_scene_rw->getCurrentStateNonConst().update();
  return planning_scene_rw->getCurrentState();
}

bool RLLMoveIfacePlanning::stateInCollision(robot_state::RobotState* state)
{
  state->update(true);
  collision_detection::CollisionRequest request;
  request.distance = true;
  request.verbose = true;
  request.contacts = true;
  request.max_contacts = 1;
  request.max_contacts_per_pair = 1;

  collision_detection::CollisionResult result;
  result.clear();
  planning_scene_->checkCollision(request, result, *state, acm_);

  // TODO(mark): outputting the collision info here is redundant if a verbose CollisionRequest is used.
  // However, it might be usefull if this info is printed/published somewhere in the future
  for (const auto& collision : result.contacts)
  {
    for (const auto& contact : collision.second)
    {
      std::string ns_name = contact.body_name_1 + "=" + contact.body_name_2;
      ROS_INFO("At most one collision detected between: %s", ns_name.c_str());
    }
  }

  bool distance_too_close = result.distance >= 0.0 && result.distance < 0.001;
  if (distance_too_close)
  {
    ROS_INFO("Distance (%.4f) too small => treated as collision", result.distance);
  }

  // There is either a collision or the distance between the robot
  // and the nearest collision object is less than 1mm.
  // Positions that are that close to a collision are disallowed
  // because the robot may end up being in collision when it
  // moves into the goal pose and ends up in a slightly different
  // position.
  return (result.collision || distance_too_close);
}

void RLLMoveIfacePlanning::updateCollisionEntry(const std::string& link_1, const std::string& link_2,
                                                bool allow_collision)
{
  ROS_INFO("Update acm collision entry: %s and %s, can collide: %d", link_1.c_str(), link_2.c_str(), allow_collision);
  planning_scene_monitor::LockedPlanningSceneRW planning_scene_rw(planning_scene_monitor_);
  planning_scene_rw->getAllowedCollisionMatrixNonConst().setEntry(link_1, link_2, allow_collision);
  // we need a local copy because checkCollision doesn't automatically use the
  // updated collision matrix from the planning scene
  acm_ = planning_scene_rw->getAllowedCollisionMatrixNonConst();
}

RLLErrorCode RLLMoveIfacePlanning::computeLinearPathArmangle(const std::vector<geometry_msgs::Pose>& waypoints_pose,
                                                             const std::vector<double>& waypoints_arm_angles,
                                                             const std::vector<double>& ik_seed_state,
                                                             std::vector<robot_state::RobotStatePtr>* path)
{
  double last_valid_percentage = 0.0;
  getPathIK(waypoints_pose, waypoints_arm_angles, ik_seed_state, path, &last_valid_percentage);

  std::vector<robot_state::RobotStatePtr> traj;

  // test for jump_threshold
  moveit::core::JumpThreshold thresh(DEFAULT_LINEAR_JUMP_THRESHOLD);
  last_valid_percentage *= robot_state::RobotState::testJointSpaceJump(manip_joint_model_group_, *path, thresh);

  if (last_valid_percentage < 1.0 && last_valid_percentage > 0.0)
  {  // TODO(updim): visualize path until collision
    ROS_ERROR("only achieved to compute %f %% of the requested path", last_valid_percentage * 100.0);
    return RLLErrorCode::ONLY_PARTIAL_PATH_PLANNED;
  }
  if (last_valid_percentage <= 0.0)
  {
    ROS_ERROR("path planning completely failed");
    return RLLErrorCode::MOVEIT_PLANNING_FAILED;
  }

  return RLLErrorCode::SUCCESS;
}

void RLLMoveIfacePlanning::getPathIK(const std::vector<geometry_msgs::Pose>& waypoints_pose,
                                     const std::vector<double>& ik_seed_state,
                                     std::vector<robot_state::RobotStatePtr>* path, double* last_valid_percentage)
{
  RLLInvKinOptions ik_options;
  RLLKinSolutions ik_solutions;
  RLLKinSeedState seed_state;
  robot_state::RobotState tmp_state = getCurrentRobotState();
  std::vector<double> sol(RLL_NUM_JOINTS);
  std::vector<double> seed_tmp_1 = ik_seed_state;
  std::vector<double> seed_tmp_2 = ik_seed_state;

  ik_options.joint_velocity_scaling_factor = DEFAULT_VELOCITY_SCALING_FACTOR;
  ik_options.joint_acceleration_scaling_factor = DEFAULT_ACCELERATION_SCALING_FACTOR;
  ik_options.global_configuration_mode = RLLInvKinOptions::KEEP_CURRENT_GLOBAL_CONFIG;

  tmp_state.setJointGroupPositions(manip_joint_model_group_, ik_seed_state);
  path->push_back(std::make_shared<moveit::core::RobotState>(tmp_state));

  for (size_t i = 1; i < waypoints_pose.size(); i++)
  {
    seed_state.clear();
    seed_state.emplace_back(seed_tmp_1);
    seed_state.emplace_back(seed_tmp_2);

    RLLKinMsg result = kinematics_plugin_->callRLLIK(waypoints_pose[i], seed_state, &ik_solutions, ik_options);
    if (result.error())
    {
      // TODO(wolfgang): also print pose where IK failed
      *last_valid_percentage = static_cast<double>(i) / static_cast<double>(waypoints_pose.size());
      return;
    }

    ik_solutions.front().getJoints(&sol);
    seed_tmp_2 = seed_tmp_1;
    seed_tmp_1 = sol;
    tmp_state.setJointGroupPositions(manip_joint_model_group_, sol);
    path->push_back(std::make_shared<moveit::core::RobotState>(tmp_state));
  }

  *last_valid_percentage = 1.0;
}

void RLLMoveIfacePlanning::getPathIK(const std::vector<geometry_msgs::Pose>& waypoints_pose,
                                     const std::vector<double>& waypoints_arm_angles,
                                     const std::vector<double>& ik_seed_state,
                                     std::vector<robot_state::RobotStatePtr>* path, double* last_valid_percentage)
{
  robot_state::RobotState tmp_state = getCurrentRobotState();
  std::vector<double> sol(RLL_NUM_JOINTS);
  std::vector<double> seed_tmp = ik_seed_state;

  if (waypoints_pose.size() != waypoints_arm_angles.size())
  {
    ROS_ERROR("getPathIK: size of waypoints and arm angles vectors do not match");
    *last_valid_percentage = 0.0;
    return;
  }

  tmp_state.setJointGroupPositions(manip_joint_model_group_, ik_seed_state);
  path->push_back(std::make_shared<moveit::core::RobotState>(tmp_state));

  for (size_t i = 1; i < waypoints_pose.size(); i++)
  {
    moveit_msgs::MoveItErrorCodes error_code;
    bool success = kinematics_plugin_->getPositionIKarmangle(waypoints_pose[i], seed_tmp, &sol, &error_code,
                                                             waypoints_arm_angles[i]);
    if (!success)
    {
      *last_valid_percentage = static_cast<double>(i) / static_cast<double>(waypoints_pose.size());
      return;
    }

    seed_tmp = sol;
    tmp_state.setJointGroupPositions(manip_joint_model_group_, sol);
    path->push_back(std::make_shared<moveit::core::RobotState>(tmp_state));
  }

  *last_valid_percentage = 1.0;
}

RLLErrorCode RLLMoveIfacePlanning::interpolatePosesLinear(const geometry_msgs::Pose& start,
                                                          const geometry_msgs::Pose& end,
                                                          std::vector<geometry_msgs::Pose>* waypoints,
                                                          const size_t steps_arm_angle)
{
  // most parts adapted from Moveit's computeCartesianPath()
  // https://github.com/ros-planning/moveit/blob/master/moveit_core/robot_state/src/cartesian_interpolator.cpp#L99

  Eigen::Isometry3d start_pose;
  tf::poseMsgToEigen(start, start_pose);

  Eigen::Isometry3d target_pose;
  tf::poseMsgToEigen(end, target_pose);

  Eigen::Quaterniond start_quaternion(start_pose.linear());
  Eigen::Quaterniond target_quaternion(target_pose.linear());

  double rotation_distance = start_quaternion.angularDistance(target_quaternion);
  double translation_distance = (target_pose.translation() - start_pose.translation()).norm();

  // decide how many steps we will need for this trajectory
  std::size_t translation_steps = 0;
  if (DEFAULT_LINEAR_EEF_STEP > 0.0)
  {
    translation_steps = floor(translation_distance / DEFAULT_LINEAR_EEF_STEP);
  }

  std::size_t rotation_steps = 0;
  if (DEFAULT_ROTATION_EEF_STEP > 0.0)
  {
    rotation_steps = floor(rotation_distance / DEFAULT_ROTATION_EEF_STEP);
  }

  std::size_t steps = std::max(translation_steps, std::max(steps_arm_angle, rotation_steps)) + 1;
  ROS_INFO_STREAM("interpolated path with " << steps << " waypoints");
  if (steps < LINEAR_MIN_STEPS_FOR_JUMP_THRESH)
  {
    ROS_WARN("Linear motions that cover a distance of less than 10 mm or sole end-effector rotations with less than 10 "
             "degrees are currently not supported."
             " Please use the 'move_ptp' service instead.");
    return RLLErrorCode::TOO_FEW_WAYPOINTS;
  }

  waypoints->clear();
  waypoints->push_back(start);

  geometry_msgs::Pose tmp;

  for (std::size_t i = 1; i <= steps; ++i)  // slerp-interpolation
  {
    double percentage = static_cast<double>(i) / static_cast<double>(steps);

    Eigen::Isometry3d pose(start_quaternion.slerp(percentage, target_quaternion));
    pose.translation() = percentage * target_pose.translation() + (1 - percentage) * start_pose.translation();

    tf::poseEigenToMsg(pose, tmp);
    waypoints->push_back(tmp);
  }

  return RLLErrorCode::SUCCESS;
}

size_t RLLMoveIfacePlanning::numStepsArmAngle(const double start, const double end)
{
  if (end < start)
  {
    return floor((2 * M_PI + end - start) / DEFAULT_ROTATION_EEF_STEP);
  }

  return floor((end - start) / DEFAULT_ROTATION_EEF_STEP);
}

void RLLMoveIfacePlanning::interpolateArmangleLinear(const double start, const double end, const int dir, const int n,
                                                     std::vector<double>* arm_angles)
{
  double step_size_arm_angle;

  // calculate step_size depending on direction and number of points
  if (dir == 1 && end < start)
  {
    step_size_arm_angle = (2 * M_PI + end - start) / (n - 1);
  }
  else if (dir == -1 && end > start)
  {
    step_size_arm_angle = (-2 * M_PI + end - start) / (n - 1);
  }
  else
  {
    step_size_arm_angle = (end - start) / (n - 1);
  }

  // fill vector
  arm_angles->clear();
  for (int i = 0; i < n; i++)
  {
    arm_angles->push_back(start + i * step_size_arm_angle);
  }
}

void RLLMoveIfacePlanning::transformPoseForIK(geometry_msgs::Pose* pose)
{
  tf::Transform world_to_ee, base_to_tip;

  tf::poseMsgToTF(*pose, world_to_ee);
  base_to_tip = base_to_world_ * world_to_ee * ee_to_tip_;
  tf::poseTFToMsg(base_to_tip, *pose);
}

void RLLMoveIfacePlanning::transformPoseFromFK(geometry_msgs::Pose* pose)
{
  tf::Transform base_to_tip, world_to_ee;

  tf::poseMsgToTF(*pose, base_to_tip);
  world_to_ee = base_to_world_.inverse() * base_to_tip * ee_to_tip_.inverse();
  tf::poseTFToMsg(world_to_ee, *pose);
}

bool RLLMoveIfacePlanning::armangleInRange(double arm_angle)
{
  if (arm_angle < -M_PI || arm_angle > M_PI)
  {
    ROS_WARN("requested arm angle is out of range [-Pi,Pi]");
    return false;
  }

  return true;
}

bool RLLMoveIfacePlanning::getKinematicsSolver()
{
  // Load instance of solver and kinematics plugin
  const kinematics::KinematicsBaseConstPtr& solver = manip_joint_model_group_->getSolverInstance();
  kinematics_plugin_ = std::dynamic_pointer_cast<const rll_moveit_kinematics::RLLMoveItKinematicsPlugin>(solver);
  if (!kinematics_plugin_)
  {
    ROS_FATAL("RLLMoveItKinematicsPlugin could not be loaded");
    return false;
  }

  return true;
}

bool RLLMoveIfacePlanning::initConstTransforms()
{
  // Static Transformations between frames
  tf2_ros::Buffer tf_buffer;
  tf2_ros::TransformListener tf_listener(tf_buffer);
  geometry_msgs::TransformStamped ee_to_tip_stamped, base_to_world_stamped;
  std::string world_frame = manip_move_group_.getPlanningFrame();
#if ROS_VERSION_MINIMUM(1, 14, 3)  // Melodic
                                   // leave world_frame as is
#else                              // Kinetic and older
  // remove slash -> TODO(mark): there might not even be a slash! e.g. starting manually with ROS_NAMESPACE=iiwa
  world_frame.erase(0, 1);
#endif
  try
  {
    ee_to_tip_stamped = tf_buffer.lookupTransform(manip_move_group_.getEndEffectorLink(),
                                                  kinematics_plugin_->getTipFrame(), ros::Time(0), ros::Duration(1.0));
    base_to_world_stamped =
        tf_buffer.lookupTransform(kinematics_plugin_->getBaseFrame(), world_frame, ros::Time(0), ros::Duration(1.0));
  }
  catch (tf2::TransformException& ex)
  {
    ROS_FATAL("%s", ex.what());
    // abortDueToCriticalFailure(); -> pure virtual => NOT set in VTABLE YET!!
    return false;
  }

  tf::transformMsgToTF(ee_to_tip_stamped.transform, ee_to_tip_);
  tf::transformMsgToTF(base_to_world_stamped.transform, base_to_world_);
  return true;
}

bool RLLMoveIfacePlanning::modifyLinTrajectory(moveit_msgs::RobotTrajectory* trajectory)
{
  // TODO(wolfgang): do ptp parametrization as long as we don't have a working cartesian parametrization
  ROS_WARN("cartesian time parametrization is not yet supported, applying PTP parametrization instead");
  return modifyPtpTrajectory(trajectory);
}
