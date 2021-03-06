/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein
*********************************************************************/
#include <navfn/navfn_ros.h>
#include <pluginlib/class_list_macros.h>
#include <tf/transform_listener.h>
#include <costmap_2d/cost_values.h>
#include <costmap_2d/costmap_2d.h>

#include <pcl_conversions/pcl_conversions.h>

//register this planner as a BaseGlobalPlanner plugin
PLUGINLIB_EXPORT_CLASS(navfn::NavfnROS, nav_core::BaseGlobalPlanner)

namespace navfn {

  NavfnROS::NavfnROS() 
    : costmap_(NULL),  planner_(), initialized_(false), allow_unknown_(true) {}

  NavfnROS::NavfnROS(std::string name, costmap_2d::Costmap2DROS* costmap_ros)
    : costmap_(NULL),  planner_(), initialized_(false), allow_unknown_(true) {
      //initialize the planner
      initialize(name, costmap_ros);
  }

  NavfnROS::NavfnROS(std::string name, costmap_2d::Costmap2D* costmap, std::string global_frame)
    : costmap_(NULL),  planner_(), initialized_(false), allow_unknown_(true) {
      //initialize the planner
      initialize(name, costmap, global_frame);
  }

  void NavfnROS::initialize(std::string name, costmap_2d::Costmap2D* costmap, std::string global_frame){
    if(!initialized_){
      ROS_DEBUG("entering inside loop of initiialize navfnros");
      costmap_ = costmap;
      global_frame_ = global_frame;
      planner_ = boost::shared_ptr<NavFn>(new NavFn(costmap_->getSizeInCellsX(), costmap_->getSizeInCellsY()));

      ros::NodeHandle private_nh("~/" + name);

      plan_pub_ = private_nh.advertise<nav_msgs::Path>("plan", 5);

      private_nh.param("visualize_potential", visualize_potential_, false);

      //if we're going to visualize the potential array we need to advertise
      if(visualize_potential_)
        potarr_pub_.advertise(private_nh, "potential", 1);

      private_nh.param("allow_unknown", allow_unknown_, true);
      private_nh.param("planner_window_x", planner_window_x_, 0.0);
      private_nh.param("planner_window_y", planner_window_y_, 0.0);
      private_nh.param("default_tolerance", default_tolerance_, 0.0);

      private_nh.param("subgoal_tolerance", subgoal_tolerance_, 1.0);

      //get the tf prefix
      ros::NodeHandle prefix_nh;
      tf_prefix_ = tf::getPrefixParam(prefix_nh);

      make_plan_srv_ =  private_nh.advertiseService("make_plan", &NavfnROS::makePlanService, this);
      subgoal_pose_sub_ = private_nh.subscribe("/initialpose",1, &NavfnROS::subgoalCallback, this); // loop up how to do this properly, use this?

      subgoal_pub_ = private_nh.advertise<geometry_msgs::PoseArray>("/planned_subgoals", 1);
      v_subgoals_.header.frame_id = "map";


      initialized_ = true;
    }
    else
      ROS_WARN("This planner has already been initialized, you can't call it twice, doing nothing");
  }

  void NavfnROS::initialize(std::string name, costmap_2d::Costmap2DROS* costmap_ros){
    initialize(name, costmap_ros->getCostmap(), costmap_ros->getGlobalFrameID());
  }

  bool NavfnROS::validPointPotential(const geometry_msgs::Point& world_point){
    return validPointPotential(world_point, default_tolerance_);
  }

  bool NavfnROS::validPointPotential(const geometry_msgs::Point& world_point, double tolerance){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized yet, but it is being used, please call initialize() before use");
      return false;
    }

    double resolution = costmap_->getResolution();
    geometry_msgs::Point p;
    p = world_point;

    p.y = world_point.y - tolerance;

    while(p.y <= world_point.y + tolerance){
      p.x = world_point.x - tolerance;
      while(p.x <= world_point.x + tolerance){
        double potential = getPointPotential(p);
        if(potential < POT_HIGH){
          return true;
        }
        p.x += resolution;
      }
      p.y += resolution;
    }

    return false;
  }

  double NavfnROS::getPointPotential(const geometry_msgs::Point& world_point){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized yet, but it is being used, please call initialize() before use");
      return -1.0;
    }

    unsigned int mx, my;
    if(!costmap_->worldToMap(world_point.x, world_point.y, mx, my))
      return DBL_MAX;

    unsigned int index = my * planner_->nx + mx;
    return planner_->potarr[index];
  }

  bool NavfnROS::computePotential(const geometry_msgs::Point& world_point){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized yet, but it is being used, please call initialize() before use");
      return false;
    }

    //make sure to resize the underlying array that Navfn uses
    planner_->setNavArr(costmap_->getSizeInCellsX(), costmap_->getSizeInCellsY());
    planner_->setCostmap(costmap_->getCharMap(), true, allow_unknown_);

    unsigned int mx, my;
    if(!costmap_->worldToMap(world_point.x, world_point.y, mx, my))
      return false;

    int map_start[2];
    map_start[0] = 0;
    map_start[1] = 0;

    int map_goal[2];
    map_goal[0] = mx;
    map_goal[1] = my;

    planner_->setStart(map_start);
    planner_->setGoal(map_goal);

    return planner_->calcNavFnDijkstra();
  }

  void NavfnROS::clearRobotCell(const tf::Stamped<tf::Pose>& global_pose, unsigned int mx, unsigned int my){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized yet, but it is being used, please call initialize() before use");
      return;
    }

    //set the associated costs in the cost map to be free
    costmap_->setCost(mx, my, costmap_2d::FREE_SPACE);
  }

  bool NavfnROS::makePlanService(nav_msgs::GetPlan::Request& req, nav_msgs::GetPlan::Response& resp){
    makePlan(req.start, req.goal, resp.plan.poses);

    resp.plan.header.stamp = ros::Time::now();
    resp.plan.header.frame_id = global_frame_;

    return true;
  } 

  void NavfnROS::mapToWorld(double mx, double my, double& wx, double& wy) {
    wx = costmap_->getOriginX() + mx * costmap_->getResolution();
    wy = costmap_->getOriginY() + my * costmap_->getResolution();
  }

  bool NavfnROS::makePlan(const geometry_msgs::PoseStamped& start, 
      const geometry_msgs::PoseStamped& goal, std::vector<geometry_msgs::PoseStamped>& plan){
    return makePlan(start, goal, default_tolerance_, plan);
  }
  /*
  bool isBacktracking(std::vector<geometry_msgs::PoseStamped> to_subgoal, std::vector<geometry_msgs::PoseStamped> from_subgoal)
  {

    int max_number_points = 10;
    if(to_subgoal.size() < max_number_points || from_subgoal.size() < max_number_points)
    {
      ROS_WARN("Too small vector to evaluate backtracking, assuming no backtrack");
      return false;
    }
    for(int i = 0; i < max_number_points; i++)
    {
      to_subgoal.at(to_subgoal.end());
      to_subgoal[0].pose.position.x;

      //fabs(std::sqrt(pow(subgoal_pose.position.x - wx,2)+pow(subgoal_pose.position.y - wy,2))) < 1.0);

    }

  }*/

  bool NavfnROS::makePlan(const geometry_msgs::PoseStamped& start, const geometry_msgs::PoseStamped& goal, double tolerance, std::vector<geometry_msgs::PoseStamped>& plan){
    boost::mutex::scoped_lock lock(mutex_);
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized yet, but it is being used, please call initialize() before use");
      return false;
    }
    ROS_DEBUG("entering makePlan");

    //clear the plan, just in case
    plan.clear();

    ros::NodeHandle n;

    double wx = start.pose.position.x;
    double wy = start.pose.position.y;

    double wx_curr = start.pose.position.x;
    double wy_curr = start.pose.position.y;
    geometry_msgs::PoseArray test;
    geometry_msgs::PoseStamped subgoal = goal;  // This overriden by the vector's poses to get the correct header.
    geometry_msgs::PoseStamped substart = start; // this will be the start for the next subgoal or the goal

    std::vector<geometry_msgs::Pose>::iterator it;
    for(it = v_subgoals_.poses.begin(); it != v_subgoals_.poses.end(); )
    {
      ROS_INFO_STREAM("pose x: " << (*it).position.x << " y: "<< (*it).position.y);

      if(fabs(std::sqrt(pow((*it).position.x - wx,2)+pow((*it).position.y - wy,2))) < subgoal_tolerance_) // If within goal tolerance (meters).
      {
        it = v_subgoals_.poses.erase(it); // returns next element
        subgoal_pub_.publish(v_subgoals_);
        continue;
      }
      else
      {
        subgoal.pose = *it; // set first reachable goal
        if(makePlanSubgoal(substart, subgoal, 0, plan))
        {
         substart.pose = *it; // set goal as new start position
         ROS_INFO("Succesfully calculated path to subgoal, length %d", plan.size());
         break; // done calculating
        }
        else
        {
         ROS_ERROR("Failed to compute plan for subgoal"); // will try to use previous point as reference for the next goal
         // will try for next goal instead
         it = v_subgoals_.poses.erase(it); // returns next element
         subgoal_pub_.publish(v_subgoals_);
        }
      }

    }

    //until tf can handle transforming things that are way in the past... we'll require the goal to be in our global frame
    if(tf::resolve(tf_prefix_, goal.header.frame_id) != tf::resolve(tf_prefix_, global_frame_)){
      ROS_ERROR("The goal pose passed to this planner must be in the %s frame.  It is instead in the %s frame.", 
                tf::resolve(tf_prefix_, global_frame_).c_str(), tf::resolve(tf_prefix_, goal.header.frame_id).c_str());
      return false;
    }

    if(tf::resolve(tf_prefix_, start.header.frame_id) != tf::resolve(tf_prefix_, global_frame_)){
      ROS_ERROR("The start pose passed to this planner must be in the %s frame.  It is instead in the %s frame.", 
                tf::resolve(tf_prefix_, global_frame_).c_str(), tf::resolve(tf_prefix_, start.header.frame_id).c_str());
      return false;
    }

     wx = substart.pose.position.x;
     wy = substart.pose.position.y;

    unsigned int mx, my;
    if(!costmap_->worldToMap(wx, wy, mx, my)){
      ROS_WARN("The robot's start position is off the global costmap. Planning will always fail, are you sure the robot has been properly localized?");
      return false;
    }

    //clear the starting cell within the costmap because we know it can't be an obstacle
    tf::Stamped<tf::Pose> start_pose;
    tf::poseStampedMsgToTF(substart, start_pose);
    clearRobotCell(start_pose, mx, my);

    //make sure to resize the underlying array that Navfn uses
    planner_->setNavArr(costmap_->getSizeInCellsX(), costmap_->getSizeInCellsY());
    planner_->setCostmap(costmap_->getCharMap(), true, allow_unknown_);

    int map_start[2];
    map_start[0] = mx;
    map_start[1] = my;

    wx = goal.pose.position.x;
    wy = goal.pose.position.y;

    if(!costmap_->worldToMap(wx, wy, mx, my)){
      if(tolerance <= 0.0){
        ROS_WARN_THROTTLE(1.0, "The goal sent to the navfn planner is off the global costmap. Planning will always fail to this goal.");
        return false;
      }
      mx = 0;
      my = 0;
    }

    int map_goal[2];
    map_goal[0] = mx;
    map_goal[1] = my;

    planner_->setStart(map_goal);
    planner_->setGoal(map_start);
    // No idea why they decide to flip goal and start but my guess is that Dijkstra solves from goal to current position.

    planner_->calcNavFnDijkstra(true);

    double resolution = costmap_->getResolution();
    geometry_msgs::PoseStamped p, best_pose;
    p = goal;

    bool found_legal = false;
    double best_sdist = DBL_MAX;

    p.pose.position.y = goal.pose.position.y - tolerance;

    while(p.pose.position.y <= goal.pose.position.y + tolerance){
      p.pose.position.x = goal.pose.position.x - tolerance;
      while(p.pose.position.x <= goal.pose.position.x + tolerance){
        double potential = getPointPotential(p.pose.position);
        double sdist = sq_distance(p, goal);
        if(potential < POT_HIGH && sdist < best_sdist){
          best_sdist = sdist;
          best_pose = p;
          found_legal = true;
        }
        p.pose.position.x += resolution;
      }
      p.pose.position.y += resolution;
    }
    if( it == v_subgoals_.poses.end()) // if subgoals are done
    {
      if(found_legal){
        //extract the plan
        std::vector<geometry_msgs::PoseStamped> plan_to_goal;
        if(getPlanFromPotential(best_pose, plan_to_goal)){
          //make sure the goal we push on has the same timestamp as the rest of the plan
          geometry_msgs::PoseStamped goal_copy = best_pose;
          goal_copy.header.stamp = ros::Time::now();
          ROS_INFO("Succesfully calculated path to goal, length %d", plan_to_goal.size());
          //plan.reserve( plan.size() + plan_to_goal.size() ); // preallocate memory
          plan.insert( plan.end(), plan_to_goal.begin(), plan_to_goal.end() ); // combines plans
          plan.push_back(goal_copy);
        }
        else{
          ROS_ERROR("Failed to get a plan from potential when a legal potential was found. This shouldn't happen.");
        }
      }

      if (visualize_potential_){
        //publish potential array
        pcl::PointCloud<PotarrPoint> pot_area;
        pot_area.header.frame_id = global_frame_;
        pot_area.points.clear();
        std_msgs::Header header;
        pcl_conversions::fromPCL(pot_area.header, header);
        header.stamp = ros::Time::now();
        pot_area.header = pcl_conversions::toPCL(header);

        PotarrPoint pt;
        float *pp = planner_->potarr;
        double pot_x, pot_y;
        for (unsigned int i = 0; i < (unsigned int)planner_->ny*planner_->nx ; i++)
        {
          if (pp[i] < 10e7)
          {
            mapToWorld(i%planner_->nx, i/planner_->nx, pot_x, pot_y);
            pt.x = pot_x;
            pt.y = pot_y;
            pt.z = pp[i]/pp[planner_->start[1]*planner_->nx + planner_->start[0]]*20;
            pt.pot_value = pp[i];
            pot_area.push_back(pt);
          }
        }
        potarr_pub_.publish(pot_area);
      }

      //publish the plan for visualization purposes
      publishPlan(plan, 0.0, 1.0, 0.0, 0.0);
      return !plan.empty();
    }
    return !plan.empty();
  }
  bool NavfnROS::makePlanSubgoal(const geometry_msgs::PoseStamped& start,
      const geometry_msgs::PoseStamped& goal, double tolerance, std::vector<geometry_msgs::PoseStamped>& plan){

    ROS_DEBUG("entering makePlanSubgoal");

    //clear the plan, just in case
    plan.clear();

    ros::NodeHandle n; // probably remove this
    double wx = start.pose.position.x;
    double wy = start.pose.position.y;

    /* we let the normal makePlan loop handle the frame issues as these header names are passed over from there
     //until tf can handle transforming things that are way in the past... we'll require the goal to be in our global frame
    if(tf::resolve(tf_prefix_, goal.header.frame_id) != tf::resolve(tf_prefix_, global_frame_)){
      ROS_ERROR("The goal pose passed to this planner must be in the %s frame.  It is instead in the %s frame.",
                tf::resolve(tf_prefix_, global_frame_).c_str(), tf::resolve(tf_prefix_, goal.header.frame_id).c_str());
      return false;
    }

    if(tf::resolve(tf_prefix_, start.header.frame_id) != tf::resolve(tf_prefix_, global_frame_)){
      ROS_ERROR("The start pose passed to this planner must be in the %s frame.  It is instead in the %s frame.",
                tf::resolve(tf_prefix_, global_frame_).c_str(), tf::resolve(tf_prefix_, start.header.frame_id).c_str());
      return false;
    }*/



    unsigned int mx, my;
    if(!costmap_->worldToMap(wx, wy, mx, my)){
      ROS_WARN("The robot's sub_goal start position is off the global costmap. Planning will always fail, are you sure the robot has been properly localized?");
      return false;
    }

    //clear the starting cell within the costmap because we know it can't be an obstacle
    tf::Stamped<tf::Pose> start_pose;
    tf::poseStampedMsgToTF(start, start_pose);
    clearRobotCell(start_pose, mx, my);

    //make sure to resize the underlying array that Navfn uses
    planner_->setNavArr(costmap_->getSizeInCellsX(), costmap_->getSizeInCellsY());
    planner_->setCostmap(costmap_->getCharMap(), true, allow_unknown_);

    int map_start[2];
    map_start[0] = mx;
    map_start[1] = my;

    wx = goal.pose.position.x;
    wy = goal.pose.position.y;

    if(!costmap_->worldToMap(wx, wy, mx, my)){
      if(tolerance <= 0.0){
        ROS_WARN_THROTTLE(1.0, "The goal sent to the navfn planner is off the global costmap. Planning will always fail to this goal.");
        return false;
      }
      mx = 0;
      my = 0;
    }

    int map_goal[2];
    map_goal[0] = mx;
    map_goal[1] = my;

    planner_->setStart(map_goal);
    planner_->setGoal(map_start);

    planner_->calcNavFnDijkstra(true);

    double resolution = costmap_->getResolution();
    geometry_msgs::PoseStamped p, best_pose;
    p = goal;

    bool found_legal = false;
    double best_sdist = DBL_MAX;

    p.pose.position.y = goal.pose.position.y - tolerance;

    while(p.pose.position.y <= goal.pose.position.y + tolerance){
      p.pose.position.x = goal.pose.position.x - tolerance;
      while(p.pose.position.x <= goal.pose.position.x + tolerance){
        double potential = getPointPotential(p.pose.position);
        double sdist = sq_distance(p, goal);
        if(potential < POT_HIGH && sdist < best_sdist){
          best_sdist = sdist;
          best_pose = p;
          found_legal = true;
        }
        p.pose.position.x += resolution;
      }
      p.pose.position.y += resolution;
    }

    if(found_legal){
      //extract the plan
      if(getPlanFromPotential(best_pose, plan)){
        //make sure the goal we push on has the same timestamp as the rest of the plan
        geometry_msgs::PoseStamped goal_copy = best_pose;
        goal_copy.header.stamp = ros::Time::now();
        plan.push_back(goal_copy);
      }
      else{
        ROS_ERROR("Failed to get a plan from potential when a legal potential was found. This shouldn't happen.");
      }
    }

    if (visualize_potential_){
      //publish potential array
      pcl::PointCloud<PotarrPoint> pot_area;
      pot_area.header.frame_id = global_frame_;
      pot_area.points.clear();
      std_msgs::Header header;
      pcl_conversions::fromPCL(pot_area.header, header);
      header.stamp = ros::Time::now();
      pot_area.header = pcl_conversions::toPCL(header);

      PotarrPoint pt;
      float *pp = planner_->potarr;
      double pot_x, pot_y;
      for (unsigned int i = 0; i < (unsigned int)planner_->ny*planner_->nx ; i++)
      {
        if (pp[i] < 10e7)
        {
          mapToWorld(i%planner_->nx, i/planner_->nx, pot_x, pot_y);
          pt.x = pot_x;
          pt.y = pot_y;
          pt.z = pp[i]/pp[planner_->start[1]*planner_->nx + planner_->start[0]]*20;
          pt.pot_value = pp[i];
          pot_area.push_back(pt);
        }
      }
      potarr_pub_.publish(pot_area);
    }

    //publish the plan for visualization purposes
    //publishPlan(plan, 0.0, 1.0, 0.0, 0.0);

    return !plan.empty();
  }
  void NavfnROS::subgoalCallback(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr &subgoal)
  {
    boost::mutex::scoped_lock lock(mutex_);
    v_subgoals_.poses.push_back(subgoal->pose.pose);
    ROS_INFO("Current number of subgoals %d", v_subgoals_.poses.size());
    subgoal_pub_.publish(v_subgoals_);
  }

  void NavfnROS::publishPlan(const std::vector<geometry_msgs::PoseStamped>& path, double r, double g, double b, double a){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized yet, but it is being used, please call initialize() before use");
      return;
    }

    //create a message for the plan 
    nav_msgs::Path gui_path;
    gui_path.poses.resize(path.size());

    if(!path.empty())
    {
      gui_path.header.frame_id = path[0].header.frame_id;
      gui_path.header.stamp = path[0].header.stamp;
    }

    // Extract the plan in world co-ordinates, we assume the path is all in the same frame
    for(unsigned int i=0; i < path.size(); i++){
      gui_path.poses[i] = path[i];
    }

    plan_pub_.publish(gui_path);
  }

  bool NavfnROS::getPlanFromPotential(const geometry_msgs::PoseStamped& goal, std::vector<geometry_msgs::PoseStamped>& plan){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized yet, but it is being used, please call initialize() before use");
      return false;
    }

    //clear the plan, just in case
    plan.clear();

    //until tf can handle transforming things that are way in the past... we'll require the goal to be in our global frame
    if(tf::resolve(tf_prefix_, goal.header.frame_id) != tf::resolve(tf_prefix_, global_frame_)){
      ROS_ERROR("The goal pose passed to this planner must be in the %s frame.  It is instead in the %s frame.", 
                tf::resolve(tf_prefix_, global_frame_).c_str(), tf::resolve(tf_prefix_, goal.header.frame_id).c_str());
      return false;
    }

    double wx = goal.pose.position.x;
    double wy = goal.pose.position.y;

    //the potential has already been computed, so we won't update our copy of the costmap
    unsigned int mx, my;
    if(!costmap_->worldToMap(wx, wy, mx, my)){
      ROS_WARN_THROTTLE(1.0, "The goal sent to the navfn planner is off the global costmap. Planning will always fail to this goal.");
      return false;
    }

    int map_goal[2];
    map_goal[0] = mx;
    map_goal[1] = my;

    planner_->setStart(map_goal);

    planner_->calcPath(costmap_->getSizeInCellsX() * 4);

    //extract the plan
    float *x = planner_->getPathX();
    float *y = planner_->getPathY();
    int len = planner_->getPathLen();
    ros::Time plan_time = ros::Time::now();

    for(int i = len - 1; i >= 0; --i){
      //convert the plan to world coordinates
      double world_x, world_y;
      mapToWorld(x[i], y[i], world_x, world_y);

      geometry_msgs::PoseStamped pose;
      pose.header.stamp = plan_time;
      pose.header.frame_id = global_frame_;
      pose.pose.position.x = world_x;
      pose.pose.position.y = world_y;
      pose.pose.position.z = 0.0;
      pose.pose.orientation.x = 0.0;
      pose.pose.orientation.y = 0.0;
      pose.pose.orientation.z = 0.0;
      pose.pose.orientation.w = 1.0;
      plan.push_back(pose);
    }

    //publish the plan for visualization purposes
    publishPlan(plan, 0.0, 1.0, 0.0, 0.0);
    return !plan.empty();
  }
};
