/**
 * @file      experiment.cpp
 * @brief     Experiment "State Machine" for controlling flow
 * @author    Alejandro Bordallo <alex.bordallo@ed.ac.uk>
 * @date      2015-08-11
 * @copyright (MIT) 2015 RAD-UoE Informatics
 */

// Publish plan to Youbot (Goto or cycle), list of goals
#include <experiment/experiment.hpp>

int main(int argc, char* argv[]) {
  ros::init(argc, argv, "experiment");
  ros::NodeHandle nh("experiment");
  Experiment experiment(&nh);

  std::signal(SIGINT, Experiment::interrupt);

  ros::Rate r(10);
  CLEAR();
  ROS_INFO("Experiment launch complete. Press Enter to continue or q to exit");
  experiment.waitReturn();
  // Publish goals or plans for first time
  experiment.pubGoals();
  experiment.pubPlans(false);
  // Setup Environments
  INFO("Please launch robot environments now" << std::endl);
  while (!Experiment::isInterrupted()) {
    if (experiment.checkReadyRobots()) {break;}
    ros::spinOnce();
    r.sleep();
  }
  if (experiment.robotsReady() && !Experiment::isInterrupted()) {
    INFO("All active robots ready" << std::endl);
  } else {
    WARN("Some robots are not ready!" << std::endl);
  }

  // Setup Robots
  std::vector<std::string> robots = experiment.getRobots();
  if (!Experiment::isInterrupted()) {
    for (size_t robot_no = 0; robot_no < robots.size(); ++robot_no) {
      // INFO("Please enter start goal number" << std::endl);
      // experiment.waitReturn();
      INFO("Press enter to perform setup for " << robots[robot_no] <<
           " (q to exit)" << std::endl);
      experiment.waitReturn();
      experiment.pubPlans(true);
      experiment.setPlanning(robot_no, true);
      experiment.pubPlanning();
      while (experiment.isPlanning(robot_no) && !Experiment::isInterrupted()) {
        ros::spinOnce();
        CLEAR();
        ROS_INFO_STREAM(robots[robot_no] << " setup in progress...");
        r.sleep();
      }
      INFO("Robot " << robots[robot_no] << " finished setting up" << std::endl);
    }
  }
  // Robots move to initial goal

  // Run experiment
  if (!Experiment::isInterrupted()) {
    INFO("All robots are setup for experiment. Press enter to proceed."
         << std::endl);
    experiment.waitReturn();
  }
  experiment.pubPlans(false);
  for (size_t robot_no = 0; robot_no < robots.size(); ++robot_no) {
    experiment.setPlanning(robot_no, true);
  }
  experiment.pubPlanning();
  while (ros::ok() && !Experiment::isInterrupted()) {
    experiment.progSpin();
    ros::spinOnce();
    // Publish goals or plans if they have changed
    r.sleep();
  }
  experiment.stopExperiment();

  return 0;
}

bool Experiment::interrupted_;

Experiment::Experiment(ros::NodeHandle* nh) {
  nh_ = nh;
  this->loadParams();
  this->init();
  this->rosSetup();
}

Experiment::~Experiment() {
  ros::param::del("experiment");
}

void Experiment::init() {
  Experiment::interrupted_ = false;
  robots_planning_.resize(robots_.size(), false);
  robots_ready_ = false;
  prog_ = 0;
}

void Experiment::rosSetup() {
  for (uint8_t i = 0; i < robots_.size(); ++i) {
    planning_pub_.push_back(nh_->advertise<std_msgs::Bool>
                            ("/" + robots_[i] + "/environment/planning", 1));
    planning_sub_.push_back(nh_->subscribe<std_msgs::Bool>
                            ("/" + robots_[i] + "/environment/planning", 1,
                             boost::bind(&Experiment::planningCB,
                                         this, _1, robots_[i])));
  }
  goals_pub_ = nh_->advertise<experiment_msgs::Goals>("goals", 1, true);
  plans_pub_ = nh_->advertise<experiment_msgs::Plans>("plans", 1, true);
  srv_set_goal_ = nh_->advertiseService("set_goal", &Experiment::setGoal, this);
  srv_set_plan_ = nh_->advertiseService("set_plan", &Experiment::setPlan, this);
}

void Experiment::loadParams() {
  // Store active robot names
  ros::param::get("/experiment/robots", robots_);

  // Store navigation goals
  int goal_n;
  ros::param::param("/experiment/goals/number", goal_n, 0);
  goal_no_ = static_cast<size_t>(goal_n);
  if (goal_no_ == 0) {
    ROS_ERROR("Experiment parameters not loaded properly: No Goals!");
    ros::shutdown();
  } else {
    for (size_t i = 0; i < goal_no_; ++i) {
      geometry_msgs::Pose2D goal;
      if (ros::param::has("/experiment/goals/g_" + std::to_string(i))) {
        ros::param::get("/experiment/goals/g_" +
                        std::to_string(i) + "/x", goal.x);
        ros::param::get("/experiment/goals/g_" +
                        std::to_string(i) + "/y", goal.y);
        ros::param::get("/experiment/goals/g_" +
                        std::to_string(i) + "/theta",
                        goal.theta);
        goals_.goal.push_back(goal);
      } else {
        ROS_ERROR("Incorrect number of goals!");
        ros::shutdown();
      }
    }
  }

  // Store robot navigation plans
  for (size_t i = 0; i < robots_.size(); ++i) {
    experiment_msgs::Plan plan;
    bool repeat;
    std::vector<int> sequence;
    ros::param::get("/experiment/plans/" + robots_[i] + "/repeat", repeat);
    ros::param::get("/experiment/plans/" + robots_[i] + "/sequence", sequence);
    plan.repeat = repeat;
    for (size_t i = 0; i < sequence.size(); ++i) {
      plan.sequence.push_back(sequence[i]);
    }
    plans_.plan.push_back(plan);
    // Create Setup plan (First goal, no repeat)
    plan.repeat = false;
    plan.sequence.resize(1);
    setup_plans_.plan.push_back(plan);
  }
}

void Experiment::interrupt(int s) {
  Experiment::interrupted_ = true;
}

void Experiment::pubPlanning() {
  for (size_t i = 0; i < robots_.size(); ++i) {
    std_msgs::Bool planning;
    planning.data = robots_planning_[i];
    planning_pub_[i].publish(planning);
  }
}

void Experiment::pubGoals() {
  goals_pub_.publish(goals_);
}

void Experiment::pubPlans(bool setup_plan) {
  if (setup_plan) {
    plans_pub_.publish(setup_plans_);
  } else {
    plans_pub_.publish(plans_);
  }
}

void Experiment::planningCB(const std_msgs::Bool::ConstPtr& msg,
                            const std::string& robot) {
  for (size_t i = 0; i < robots_.size(); ++i) {
    if (robot.compare(robots_[i]) == 0) {
      robots_planning_[i] = msg->data;
      break;
    }
  }
}

bool Experiment::setGoal(experiment_msgs::SetGoal::Request& req,
                         experiment_msgs::SetGoal::Response& res) {
  res.ok = true;
  if (req.goal_no < goals_.goal.size()) {
    goals_.goal[req.goal_no] = req.goal;
  } else {
    ROS_INFO("New goal at index %lu", goals_.goal.size());
    goals_.goal.push_back(req.goal);
    goal_no_ = goals_.goal.size();
  }
  return true;
}

bool Experiment::setPlan(experiment_msgs::SetPlan::Request& req,
                         experiment_msgs::SetPlan::Response& res) {
  res.ok = false;
  bool sequence_ok = true;
  for (size_t i = 0; i < robots_.size(); ++i) {
    if (req.robot.compare(robots_[i]) == 0) {
      for (size_t i = 0; i < req.plan.sequence.size(); ++i) {
        if (req.plan.sequence[i] >= goal_no_) {
          sequence_ok = false;
          break;
        }
      }
      if (sequence_ok) {
        plans_.plan[i] = req.plan;
        res.ok = true;
      } else {
        ROS_WARN("Incorrect sequence goal ids!");
      }
      break;
    }
  }
  if (!res.ok && sequence_ok) {
    ROS_WARN("Robot %s could not be found!", req.robot.c_str());
  }
  return true;
}

bool Experiment::checkReadyRobots() {
  robots_ready_ = true;
  for (size_t i = 0; i < robots_.size(); ++i) {
    if (!ros::param::has("/" + robots_[i] + "/environment/ready"))
    {robots_ready_ = false;} else {
      INFO(robots_[i] << " is ready!" << std::endl);
    }
  }
  return robots_ready_;
}

void Experiment::stopExperiment() {
  robots_planning_.assign(robots_.size(), false);
  this->pubPlanning();
  if (Experiment::interrupted_) {ROS_INFO("User stopped experiment");}
}

void Experiment::waitReturn() {
  char key;
  while (true) {
    key = std::cin.get();
    if (key == '\n') {
      break;
    } else if (key == 'q') {
      Experiment::interrupted_ = true;
      break;
    }
  }
}

void Experiment::progSpin() {
  if (prog_ == 0) {INFO("\r|"); prog_++;}
  else if (prog_ == 1) {INFO("\r/"); prog_++;}
  else if (prog_ == 2) {INFO("\r-"); prog_++;}
  else if (prog_ == 3) {INFO("\r\\"); prog_ = 0;}
  INFO(" Running")
}
