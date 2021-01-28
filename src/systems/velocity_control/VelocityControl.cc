/*
 * Copyright (C) 2020 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <map>
#include <mutex>
#include <string>

#include <ignition/common/Profiler.hh>
#include <ignition/math/Vector3.hh>
#include <ignition/plugin/Register.hh>
#include <ignition/transport/Node.hh>

#include "ignition/gazebo/components/AngularVelocityCmd.hh"
#include "ignition/gazebo/components/LinearVelocityCmd.hh"
#include "ignition/gazebo/Model.hh"

#include "VelocityControl.hh"

using namespace ignition;
using namespace gazebo;
using namespace systems;

class ignition::gazebo::systems::VelocityControlPrivate
{
  /// \brief Callback for model velocity subscription
  /// \param[in] _msg Velocity message
  public: void OnCmdVel(const ignition::msgs::Twist &_msg);

  /// \brief Callback for link velocity subscription
  /// \param[in] _msg Velocity message
  public: void OnLinkCmdVel(const ignition::msgs::Twist &_msg,
    const ignition::transport::MessageInfo &_info);

  /// \brief Update the linear and angular velocities.
  /// \param[in] _info System update information.
  /// \param[in] _ecm The EntityComponentManager of the given simulation
  /// instance.
  public: void UpdateVelocity(const ignition::gazebo::UpdateInfo &_info,
    const ignition::gazebo::EntityComponentManager &_ecm);

  /// \brief Update link velocity.
  /// \param[in] _info System update information.
  /// \param[in] _ecm The EntityComponentManager of the given simulation
  /// instance.
  public: void UpdateLinkVelocity(const ignition::gazebo::UpdateInfo &_info,
    const ignition::gazebo::EntityComponentManager &_ecm);

  /// \brief Ignition communication node.
  public: transport::Node node;

  /// \brief Model interface
  public: Model model{kNullEntity};

  /// \brief Angular velocity of a model
  public: math::Vector3d angularVelocity{0, 0, 0};

  /// \brief Linear velocity of a model
  public: math::Vector3d linearVelocity{0, 0, 0};

  /// \brief Last target velocity requested.
  public: msgs::Twist targetVel;

  /// \brief A mutex to protect the model velocity command.
  public: std::mutex mutex;

  /// \brief link names
  public: std::vector<std::string> linkNames;

  /// \brief Link entities in a model
  public: std::map<std::string, Entity> links;

  /// \brief Angular velocities of links
  public: std::map<std::string, math::Vector3d> angularVelocities;

  /// \brief Linear velocities of links
  public: std::map<std::string, math::Vector3d> linearVelocities;

  /// \brief all link velocites
  public: std::map<std::string, msgs::Twist> linkVels;
};

//////////////////////////////////////////////////
VelocityControl::VelocityControl()
  : dataPtr(std::make_unique<VelocityControlPrivate>())
{
}

//////////////////////////////////////////////////
void VelocityControl::Configure(const Entity &_entity,
    const std::shared_ptr<const sdf::Element> &_sdf,
    EntityComponentManager &_ecm,
    EventManager &/*_eventMgr*/)
{
  this->dataPtr->model = Model(_entity);

  if (!this->dataPtr->model.Valid(_ecm))
  {
    ignerr << "VelocityControl plugin should be attached to a model entity. "
           << "Failed to initialize." << std::endl;
    return;
  }

  // Subscribe to model commands
  std::string modelTopic{"/model/" + this->dataPtr->model.Name(_ecm) + "/cmd_vel"};
  if (_sdf->HasElement("topic"))
    modelTopic = _sdf->Get<std::string>("topic");
  this->dataPtr->node.Subscribe(
    modelTopic, &VelocityControlPrivate::OnCmdVel, this->dataPtr.get());
  ignmsg << "VelocityControl subscribing to twist messages on ["
         << modelTopic << "]"
         << std::endl;

  // Ugly, but needed because the sdf::Element::GetElement is not a const
  // function and _sdf is a const shared pointer to a const sdf::Element.
  auto ptr = const_cast<sdf::Element *>(_sdf.get());

  if (!ptr->HasElement("link_name"))
    return;

  sdf::ElementPtr sdfElem = ptr->GetElement("link_name");
  while (sdfElem)
  {
    this->dataPtr->linkNames.push_back(sdfElem->Get<std::string>());
    sdfElem = sdfElem->GetNextElement("link_name");
  }

  // Subscribe to link commands
  for (const auto &linkName : this->dataPtr->linkNames)
  {
    std::string linkTopic{"/model/" + this->dataPtr->model.Name(_ecm) +
                             "/link/" + linkName + "/cmd_vel"};
    this->dataPtr->node.Subscribe(
    linkTopic, &VelocityControlPrivate::OnLinkCmdVel, this->dataPtr.get());
    ignmsg << "VelocityControl subscribing to twist messages on ["
           << linkTopic << "]"
           << std::endl;
  }
}

//////////////////////////////////////////////////
void VelocityControl::PreUpdate(const ignition::gazebo::UpdateInfo &_info,
    ignition::gazebo::EntityComponentManager &_ecm)
{
  IGN_PROFILE("VelocityControl::PreUpdate");

  // \TODO(anyone) Support rewind
  if (_info.dt < std::chrono::steady_clock::duration::zero())
  {
    ignwarn << "Detected jump back in time ["
        << std::chrono::duration_cast<std::chrono::seconds>(_info.dt).count()
        << "s]. System may not work properly." << std::endl;
  }

  // Nothing left to do if paused.
  if (_info.paused)
    return;

  // update angular velocity of model
  auto modelAngularVel =
    _ecm.Component<components::AngularVelocityCmd>(
      this->dataPtr->model.Entity());

  if (modelAngularVel == nullptr)
  {
    _ecm.CreateComponent(
      this->dataPtr->model.Entity(),
      components::AngularVelocityCmd({this->dataPtr->angularVelocity}));
  }
  else
  {
    *modelAngularVel =
      components::AngularVelocityCmd({this->dataPtr->angularVelocity});
  }

  // update linear velocity of model
  auto modelLinearVel =
    _ecm.Component<components::LinearVelocityCmd>(
      this->dataPtr->model.Entity());

  if (modelLinearVel == nullptr)
  {
    _ecm.CreateComponent(
      this->dataPtr->model.Entity(),
      components::LinearVelocityCmd({this->dataPtr->linearVelocity}));
  }
  else
  {
    *modelLinearVel =
      components::LinearVelocityCmd({this->dataPtr->linearVelocity});
  }

  // If there are links, create link components
  // If the link hasn't been identified yet, look for it
  auto modelName = this->dataPtr->model.Name(_ecm);

  if (this->dataPtr->linkNames.empty())
    return;

  for (const auto &linkName : this->dataPtr->linkNames)
  {
    if (this->dataPtr->links.find(linkName) == this->dataPtr->links.end())
    {
      Entity link = this->dataPtr->model.LinkByName(_ecm, linkName);
      if (link != kNullEntity)
        this->dataPtr->links.insert({linkName, link});
      else
      {
        ignwarn << "Failed to find link [" << linkName
              << "] for model [" << modelName << "]" << std::endl;
      }
    }
  }
  if (this->dataPtr->links.empty())
    return;

  // update link velocities
  for (const auto& [linkName, link] : this->dataPtr->links)
  {
    // update angular velocity
    auto linkAngularVel = _ecm.Component<components::AngularVelocityCmd>(link);
    auto it = this->dataPtr->angularVelocities.find(linkName);
    if (it != this->dataPtr->angularVelocities.end())
    {
      if (linkAngularVel == nullptr)
        _ecm.CreateComponent(link, components::AngularVelocityCmd({it->second}));
      else
        *linkAngularVel = components::AngularVelocityCmd(it->second);
    }
    else
    {
      ignwarn << "No angular velocity found for link [" << linkName << "]" << std::endl;
    }

    // update linear velocity
    auto linkLinearVel = _ecm.Component<components::LinearVelocityCmd>(link);
    it = this->dataPtr->linearVelocities.find(linkName);
    if (it != this->dataPtr->linearVelocities.end())
    {
      if (linkLinearVel == nullptr)
        _ecm.CreateComponent(link, components::LinearVelocityCmd({it->second}));
      else
        *linkLinearVel = components::LinearVelocityCmd({it->second});
    }
    else
    {
      ignwarn << "No linear velocity found for link [" << linkName << "]" << std::endl;
    }
  }
}

//////////////////////////////////////////////////
void VelocityControl::PostUpdate(const UpdateInfo &_info,
    const EntityComponentManager &_ecm)
{
  IGN_PROFILE("VelocityControl::PostUpdate");
  // Nothing left to do if paused.
  if (_info.paused)
    return;

  // update model velocities
  this->dataPtr->UpdateVelocity(_info, _ecm);
  // update link velocities
  this->dataPtr->UpdateLinkVelocity(_info, _ecm);
}

//////////////////////////////////////////////////
void VelocityControlPrivate::UpdateVelocity(
    const ignition::gazebo::UpdateInfo &/*_info*/,
    const ignition::gazebo::EntityComponentManager &/*_ecm*/)
{
  IGN_PROFILE("VeocityControl::UpdateVelocity");

  double linVel;
  double angVel;
  {
    std::lock_guard<std::mutex> lock(this->mutex);
    linVel = this->targetVel.linear().x();
    angVel = this->targetVel.angular().z();
  }

  this->linearVelocity = math::Vector3d(
    linVel, this->targetVel.linear().y(), this->targetVel.linear().z());
  this->angularVelocity = math::Vector3d(
    this->targetVel.angular().x(), this->targetVel.angular().y(), angVel);
}

//////////////////////////////////////////////////
void VelocityControlPrivate::UpdateLinkVelocity(
    const ignition::gazebo::UpdateInfo &/*_info*/,
    const ignition::gazebo::EntityComponentManager &/*_ecm*/)
{
  IGN_PROFILE("VeocityControl::UpdateLinkVelocity");

  for (const auto& [linkName, msg] : this->linkVels)
  {
    auto linearVel = math::Vector3d(msg.linear().x(), msg.linear().y(), msg.linear().z());
    auto angularVel = math::Vector3d(msg.angular().x(), msg.angular().y(), msg.angular().z());
    this->linearVelocities.insert({linkName, linearVel});
    this->angularVelocities.insert({linkName, angularVel});
  }
}

//////////////////////////////////////////////////
void VelocityControlPrivate::OnCmdVel(const msgs::Twist &_msg)
{
  std::lock_guard<std::mutex> lock(this->mutex);
  this->targetVel = _msg;
}

//////////////////////////////////////////////////
void VelocityControlPrivate::OnLinkCmdVel(const msgs::Twist &_msg,
                                      const transport::MessageInfo &_info)
{
  for (const auto &linkName : this->linkNames)
  {
    if (_info.Topic().find(linkName) != std::string::npos)
    {
      this->linkVels.insert({linkName, _msg});
    }
  }
}

IGNITION_ADD_PLUGIN(VelocityControl,
                    ignition::gazebo::System,
                    VelocityControl::ISystemConfigure,
                    VelocityControl::ISystemPreUpdate,
                    VelocityControl::ISystemPostUpdate)

IGNITION_ADD_PLUGIN_ALIAS(VelocityControl,
                          "ignition::gazebo::systems::VelocityControl")
