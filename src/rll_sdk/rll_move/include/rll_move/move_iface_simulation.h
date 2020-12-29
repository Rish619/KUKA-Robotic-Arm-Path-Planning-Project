/*
 * This file is part of the Robot Learning Lab SDK
 *
 * Copyright (C) 2018 Wolfgang Wiedmeyer <wolfgang.wiedmeyer@kit.edu>
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

#ifndef RLL_MOVE_MOVE_IFACE_SIMULATION_H
#define RLL_MOVE_MOVE_IFACE_SIMULATION_H

#include <rll_move/move_iface_planning.h>

class RLLSimulationMoveIface : public virtual RLLMoveIfacePlanning
{
public:
  explicit RLLSimulationMoveIface() = default;
  ~RLLSimulationMoveIface() override = default;

protected:
  bool modifyPtpTrajectory(moveit_msgs::RobotTrajectory* trajectory) override;
};

#endif  // RLL_MOVE_MOVE_IFACE_SIMULATION_H
