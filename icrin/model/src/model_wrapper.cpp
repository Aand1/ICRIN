/**
 * @file      model_wrapper.cpp
 * @brief     Model Wrapper to manage modelling functions and sim wrapper
 * @author    Alejandro Bordallo <alex.bordallo@ed.ac.uk>
 * @date      2015-08-22
 * @copyright (MIT) 2015 RAD-UoE Informatics
 */

#include "model/model_wrapper.hpp"

ModelWrapper::ModelWrapper(ros::NodeHandle* nh) {
  nh_ = nh;
  robot_name_ = ros::this_node::getNamespace();
  robot_name_.erase (0, 1); // Remove 1 forward slash from robot_name
  model_name_ = ros::this_node::getName();
  model_name_.erase (0, robot_name_.length()); // Remove robot name
  this->loadParams();
  this->init();
  this->rosSetup();
  sim_wrapper_ = new SimWrapper(nh_);
}

ModelWrapper::~ModelWrapper() {
  ros::param::del(model_name_);
  if (use_rvo_lib_) {
    delete sim_wrapper_;
    sim_wrapper_ = NULL;
  }
}

void ModelWrapper::loadParams() {
  // Model Params
  if (!ros::param::has(robot_name_ + model_name_ + "/robot_model"))
  {ROS_WARN("ModelW- Robot model by default");}
  ros::param::param(robot_name_ + model_name_ + "/robot_model",
                    robot_model_, true);
  if (!ros::param::has(robot_name_ + model_name_ + "/goal_sum_prior"))
  {ROS_WARN("ModelW- Using default Model params");}
  ros::param::param(robot_name_ + model_name_ + "/goal_sum_prior",
                    goal_sum_prior_, 0.001f);
  ros::param::param(robot_name_ + model_name_ + "/goal_history_discount",
                    goal_history_discount_, 0.5f);
  ros::param::param(robot_name_ + model_name_ + "/goal_inference_history",
                    goal_inference_history_, 10);
  ros::param::param(robot_name_ + model_name_ + "/velocity_average_window",
                    velocity_average_window_, 10);
  ros::param::param(robot_name_ + model_name_ + "/prior_lambda",
                    prior_lambda_, 0.5f);
}

void ModelWrapper::init() {
  use_rvo_lib_ = true;
  // inferred_goals_history_.resize(3);
  init_liks_.resize(3, false);
  prev_prior_.resize(3);
}

void ModelWrapper::rosSetup() {
  robot_pose_sub_ = nh_->subscribe(robot_name_ + "/environment/curr_pose", 1000,
                                   &ModelWrapper::robotPoseCB, this);
  robot_goal_sub_ = nh_->subscribe(robot_name_ + "/environment/target_goal",
                                   1000, &ModelWrapper::robotGoalCB, this);
  robot_vel_sub_ = nh_->subscribe(robot_name_ + "/cmd_vel", 1000,
                                  &ModelWrapper::robotVelCB, this);
  env_data_sub_ = nh_->subscribe(robot_name_ + "/environment/data", 1000,
                                 &ModelWrapper::envDataCB, this);
  model_hyp_sub_ = nh_->subscribe(robot_name_ + "/model/hypotheses", 1000,
                                  &ModelWrapper::modelCB, this);
}

void ModelWrapper::robotPoseCB(const geometry_msgs::Pose2D::ConstPtr& msg) {
  robot_pose_ = *msg;
}

void ModelWrapper::robotGoalCB(const geometry_msgs::Pose2D::ConstPtr& msg) {
  robot_goal_ = *msg;
}

void ModelWrapper::robotVelCB(const geometry_msgs::Twist::ConstPtr& msg) {
  robot_vel_ = *msg;
}

void ModelWrapper::envDataCB(const environment_msgs::EnvironmentData::ConstPtr&
                             msg) {
  env_data_ = *msg;
}

void ModelWrapper::modelCB(const model_msgs::ModelHypotheses::ConstPtr&
                           msg) {
  hypotheses_ = *msg;
}

void ModelWrapper::runModel() {
  this->setupModel();
  this->runSims();
  this->inferGoals();
}

void ModelWrapper::inferGoals() {
  float max_acc = 1.2f;
  float ros_freq = 0.1f;
  float PI = 3.14159265358979323846f;
  bool reset_priors = false;
  // size_t inf_hist = 10;
  common_msgs::Vector2 curr_vel;
  curr_vel.x = robot_vel_.linear.x;
  curr_vel.y = robot_vel_.linear.y;
  size_t n_goals = hypotheses_.goal_hypothesis.goal_sequence.size();
  for (size_t agent = 0; agent < hypotheses_.agents.size(); ++agent) {
    std::vector<float> g_likelihoods;
    g_likelihoods.resize(n_goals);
    std::vector<common_msgs::Vector2> agent_sim_vels;
    size_t begin = (agent * n_goals);
    size_t end = begin + (n_goals);
    agent_sim_vels.insert(agent_sim_vels.begin(),
                          sequence_sim_vels.begin() + begin,
                          sequence_sim_vels.begin() + end);
    ROS_WARN_STREAM("SimVelSize: " << agent_sim_vels.size());
    for (size_t goal = 0; goal < n_goals; ++goal) {
      // if (DISPLAY_INFERENCE_VALUES) {
      ROS_INFO_STREAM("curr_vel=[" << curr_vel.x << ", " << curr_vel.y << "] " <<
                      "simVel=[" << agent_sim_vels[goal].x << ", " <<
                      agent_sim_vels[goal].y << "]" << std::endl);
      // }
      // Bivariate Gaussian
      float x = agent_sim_vels[goal].x;
      float y = agent_sim_vels[goal].y;
      float ux = curr_vel.x;
      float uy = curr_vel.y;
      float ox = (max_acc / 2) * ros_freq;  // 2 std.dev = max vel change
      float oy = (max_acc / 2) * ros_freq;
      float o2x = pow(ox, 2);
      float o2y = pow(oy, 2);
      float corr = 0.0f;
      float corr2 = pow(corr, 2.0f);
      float t1 = pow(x - ux, 2.0f) / o2x;
      float t2 = pow(y - uy, 2.0f) / o2y;
      float t3 = (2 * corr * (x - ux) * (y - uy)) / (ox * oy);
      float p = (1 / (2 * PI * ox * oy * sqrtf(1 - corr2)));
      float e = exp(-(1 / (2 * (1 - corr2))) * (t1 + t2 - t3));
      g_likelihoods[goal] = p * e;
      ROS_INFO_STREAM("Goal: " << goal << " Lik: " << g_likelihoods[goal] <<
                      " t1: " << t1 << " t2: " << t2 << " t3: " << t3);
    }

    float posterior_norm = 0.0f;
    std::vector<float> g_posteriors;
    g_posteriors.resize(n_goals);
    float uniform_prior = 1.0f / n_goals;
    for (std::size_t goal = 0; goal < n_goals; ++goal) {

      if (reset_priors || !init_liks_[goal]) {
        g_posteriors[goal] = uniform_prior;
        init_liks_[goal] = true;
      } else {
        g_posteriors[goal] = g_likelihoods[goal] * prev_prior_[goal];
      }

      posterior_norm += g_posteriors[goal];
      ROS_INFO_STREAM("GoalPosteriors" << goal << "=" <<
                      g_posteriors[goal] << " " << std::endl);
    }

    float norm_posterior = 0.0f;
    std::vector<float> norm_posteriors;
    norm_posteriors.resize(n_goals);
    for (std::size_t goal = 0; goal < n_goals; ++goal) {
      if (posterior_norm == 0) {
        norm_posterior = uniform_prior;
        ROS_INFO_STREAM("Uni: " << norm_posterior);
        // Avoiding NaNs when likelihoods are all 0
      } else {
        norm_posterior = g_posteriors[goal] / posterior_norm;
        if (norm_posterior > 0.01f) {
          prev_prior_[goal] = norm_posterior;
        } else {
          prev_prior_[goal] = 0.005f;
        }
        ROS_INFO_STREAM("Rat: " << norm_posterior);
      }
      norm_posteriors[goal] = norm_posterior;
    }
    ROS_INFO_STREAM("G0: " << norm_posteriors[0] << " G1: " << norm_posteriors[1] <<
                    " G2: " << norm_posteriors[2]);
  }

}

void ModelWrapper::setupModel() {
  std::vector<geometry_msgs::Pose2D> agent_poses_;
  std::vector<geometry_msgs::Twist> agent_vels_;
  sim_wrapper_->setRobotModel(robot_model_);
  if (robot_model_) {
    agent_poses_.push_back(robot_pose_);
    agent_vels_.push_back(robot_vel_);
    sim_wrapper_->setRobotGoal(robot_goal_);
  }
  agent_poses_.insert(agent_poses_.end(), env_data_.agent_poses.begin(),
                      env_data_.agent_poses.end());
  agent_vels_.insert(agent_vels_.end(), env_data_.agent_vels.begin(),
                     env_data_.agent_vels.end());
  sim_wrapper_->setEnvironment(agent_poses_, agent_vels_);

  sim_wrapper_->setModelAgents(hypotheses_.agents);
  if (hypotheses_.goals) {
    if (hypotheses_.goal_hypothesis.sampling) {
      sampling_sims_ = sim_wrapper_->goalSampling(
                         hypotheses_.goal_hypothesis.sample_space,
                         hypotheses_.goal_hypothesis.sample_resolution);
    } else {
      sequence_sims_ = sim_wrapper_->goalSequence(
                         hypotheses_.goal_hypothesis.goal_sequence);
    }
  }
  if (hypotheses_.awareness) {
    ROS_WARN("ModelW- Awareness modelling not implemented yet!");
  }
}

void ModelWrapper::runSims() {
  sequence_sim_vels.clear();
  if (hypotheses_.goals) {
    if (hypotheses_.goal_hypothesis.sampling) {
      ROS_WARN("ModelW- Run Goal Sampling Sims!");
    } else {
      ROS_WARN("ModelW- Run Goal Sequence Sims!");
      size_t n_goals = hypotheses_.goal_hypothesis.goal_sequence.size();
      sequence_sim_vels = sim_wrapper_->calcSimVels(sequence_sims_, n_goals);
    }
  }
  if (hypotheses_.awareness) {
    ROS_WARN("ModelW- Awareness modelling not implemented yet!");
  }
  for (size_t i = 0; i < sequence_sim_vels.size(); ++i) {
    ROS_INFO_STREAM("SimVel" << i << ": " << sequence_sim_vels[i].x <<
                    ", " << sequence_sim_vels[i].y);
  }
}