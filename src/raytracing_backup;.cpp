#include "victim_localization/raytracing.h"

RayTracing::RayTracing(view_generator_IG *vg):
  view_gen_ (vg)
{

  double fov_h, fov_v, r_max, r_min;
  ros::param::param("~fov_horizontal", fov_h, 58.0);
  ros::param::param("~fov_vertical", fov_v, 45.0);
  ros::param::param("~depth_range_max", r_max, 5.0);
  ros::param::param("~depth_range_min", r_min, 0.05);

  setCameraSettings(fov_h, fov_v, r_max, r_min);
  getCameraRotationMtxs();
}

void RayTracing::update()
{

  tree_               = view_gen_->manager_->octree_.get();
  current_pose_       = view_gen_->current_pose_;

// Update evaluation bounds
//tree_resolution_ = tree_->getResolution();

  octomap::point3d min (
        view_gen_->obj_bounds_x_min_,
        view_gen_->obj_bounds_y_min_,
        view_gen_->obj_bounds_z_min_);
  octomap::point3d max (
        view_gen_->obj_bounds_x_max_,
        view_gen_->obj_bounds_y_max_,
        view_gen_->obj_bounds_z_max_);

  tree_->setBBXMin( min );
  tree_->setBBXMax( max );

  computeRelativeRays();
}




double RayTracing::computeRelativeRays()
{
  rays_far_plane_.clear();

  double deg2rad = M_PI/180;
  double min_x = -range_max_ * tan(HFOV_deg/2 * deg2rad);
  double min_y = -range_max_ * tan(VFOV_deg/2 * deg2rad);
  double max_x =  range_max_ * tan(HFOV_deg/2 * deg2rad);
  double max_y =  range_max_ * tan(VFOV_deg/2 * deg2rad);

  double x_step = view_gen_->manager_->getResolution();
  double y_step = view_gen_->manager_->getResolution();

  for( double x = min_x; x<=max_x; x+=x_step )
  {
    for( double y = min_y; y<=max_y; y+=y_step )
    {
      Eigen::Vector3d p_far(range_max_, x, y);
      rays_far_plane_.push_back(p_far);
    }
  }
}

void RayTracing::computeRaysAtPose(geometry_msgs::Pose p)
{
  rays_far_plane_at_pose_.clear();

  Eigen::Matrix3d r_pose, rotation_mtx_;
  r_pose = pose_conversion::getRotationMatrix( p );

  // For each camera, compute the rays

  rotation_mtx_ = r_pose * camera_rotation_mtx_;

  for (int r=0; r<rays_far_plane_.size(); r++)
  {
    // Rotate ray to its final position
    Eigen::Vector3d temp = rotation_mtx_*rays_far_plane_[r];

    // Create an octomap point to later cast a ray
    octomap::point3d p_ (temp[0], temp[1], temp[2]);
    //std::cout << "for pose: "<< p << " finalpoints: "<< p_ <<std::endl;
    if (temp[2]>(current_pose_.position.z -1) && temp[2]<(current_pose_.position.z +1))
      rays_far_plane_at_pose_.push_back(p_);
  }
}

void RayTracing::getCameraRotationMtxs()
{
  tf::TransformListener tf_listener;
  tf::StampedTransform transform;

  // Keep trying until it succeeds
  // TF failures happen in the few moments after the system starts for the first time
  while(true)
  {
    try
    {
      tf_listener.lookupTransform(base_frame, camera_frame, ros::Time(0), transform);
      break; // Success, break out of the loop
    }
    catch (tf2::LookupException ex){
      ROS_ERROR("[ViewSelecterBase] %s",ex.what());
      ros::Duration(0.1).sleep();
    }
    catch (tf2::ExtrapolationException ex){
      ROS_ERROR("[ViewSelecterBase] %s",ex.what());
      ros::Duration(0.1).sleep();
    }
  }
  camera_rotation_mtx_= pose_conversion::getRotationMatrix(transform) ;
}



void RayTracing::setCameraSettings(double fov_h, double fov_v, double r_max, double r_min)
{
  HFOV_deg = fov_h;
  VFOV_deg = fov_v;
  range_max_ = r_max;
  range_min_ = r_min;
}

bool RayTracing::isNodeInBounds(octomap::OcTreeKey &key)
{
  octomap::point3d p = tree_->keyToCoord(key);
  return isPointInBounds(p);
}

bool RayTracing::isNodeFree(octomap::OcTreeNode node)
{
  if (node.getOccupancy() <= 1-tree_->getOccupancyThres())
    return true;

  return false;
}

bool RayTracing::isNodeOccupied(octomap::OcTreeNode node)
{
  if (node.getOccupancy() >= tree_->getOccupancyThres())
    return true;

  return false;
}

bool RayTracing::isNodeUnknown(octomap::OcTreeNode node)
{
  return !isNodeFree(node) && !isNodeOccupied(node);
}

bool RayTracing::isPointInBounds(octomap::point3d &p)
{
  bool result = (p.x() >= view_gen_->obj_bounds_x_min_ && p.x() <= view_gen_->obj_bounds_x_max_ &&
                 p.y() >= view_gen_->obj_bounds_y_min_ && p.y() <= view_gen_->obj_bounds_y_max_ &&
                 p.z() >= view_gen_->obj_bounds_z_min_ && p.z() <= view_gen_->obj_bounds_z_max_ );

  return result;
}



double RayTracing::getNodeOccupancy(octomap::OcTreeNode* node)
{
  double p;

  if( node==NULL )
  {
    p = 0.5;
  }
  else
  {
    p = node->getOccupancy();
  }
  return p;
}


grid_map::GridMap RayTracing::Project_3d_rayes_to_2D_plane(Pose p_)
{
  //  // Source: Borrowed partially from
  //  // https://github.com/uzh-rpg/rpg_ig_active_reconstruction/blob/master/ig_active_reconstruction_octomap/src/code_base/octomap_basic_ray_ig_calculator.inl
  update();
  computeRaysAtPose(p_);
  grid_map::GridMap Pose_map;
  Pose_map.setFrameId("map");
  Pose_map.setGeometry(Length(range_max_*2.5,range_max_*2.5), 1,Position (p_.position.x,p_.position.y));
  Pose_map.add({"temp"},0.5);  //0 for free & unknown (for time being check free only), 0.5 for unexplored,


  octomap::point3d origin (p_.position.x, p_.position.y, p_.position.z);

  std::set<octomap::OcTreeKey, OctomapKeyCompare> nodes; //all nodes in a set are UNIQUE

  for (int i=0; i<rays_far_plane_at_pose_.size(); i++)
  {
    octomap::point3d endpoint;

    // Get length of beam to the far plane of sensor
    double range = rays_far_plane_at_pose_[i].norm();
    // Get the direction of the ray
    octomap::point3d dir = rays_far_plane_at_pose_[i].normalize();

    //std::cout << "dir "  << dir << std::endl;

    // Cast through unknown cells as well as free cells
    bool found_endpoint = tree_->castRay(origin, dir, endpoint, true, range);
    if (!found_endpoint)
    {
      endpoint = origin + dir * range;
    }

    octomap::point3d start_pt, end_pt;
    bool entered_valid_range = false;
    bool exited_valid_range = false;

    //std::cout << "endpointis : " << std::endl;
    //if (found_endpoint==1) std::cout << "found; Endpoint: " << endpoint <<  std::endl;

    /* Check ray
   *
   * We first draw a ray from the UAV to the endpoint
   *
   * For generality, we assume the UAV is outside the object bounds. We only consider nodes
   * 	that are inside the object bounds, and past the near-plane of the camera.
   *
   * The ray continues until the end point.
   * If the ray exits the bounds, we stop adding nodes to IG and discard the endpoint.
   *
   */

    std::cout << "NEWRAY" << origin << std::endl;

    octomap::KeyRay ray;
    tree_->computeRayKeys( origin, endpoint, ray );

    for( octomap::KeyRay::iterator it = ray.begin() ; it!=ray.end(); ++it )
    {
      octomap::point3d p = tree_->keyToCoord(*it);

      std::cout << "this is the point from the ray: " << p << std::endl;

      if (!entered_valid_range)
      {
        // Find the first valid point
        if (isPointInBounds(p) )
        {
          entered_valid_range = true;
          start_pt = p;
          end_pt = p;
        }

        else
          continue;
      }

      else
      {
        if (isPointInBounds(p))
          end_pt = p;

        else
        {
          exited_valid_range = true;
          break; //We've exited the valid range
        }
      }

      // project the point to the Map
      octomap::OcTreeNode* node = tree_->search(*it);
      Position position(p.x(),p.y());

      double prob = getNodeOccupancy(node);
      if (prob > 0.8) Pose_map.atPosition("temp",position)=1;

      if (prob < 0.2)
        if (Pose_map.atPosition("temp",position)!=1) Pose_map.atPosition("temp",position)=0;
    }

    // Evaluate endpoint     // this is checked as the end is not checked in the for loop
  }

//   std::cout << "This is the map for pose : " << p_ << std::endl;
//  for (grid_map::GridMapIterator iterator(Pose_map); !iterator.isPastEnd(); ++iterator) {
//    Position position;
//     Pose_map.getPosition(*iterator,position);
//     std::cout << "position: " << position << " value: " << Pose_map.atPosition("temp",position)
//               << std::endl;
//  }

  return Pose_map;
}



