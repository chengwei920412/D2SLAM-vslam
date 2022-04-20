#pragma once
#include <ros/ros.h>

namespace D2VINS {
class D2EstimatorState;
class D2Estimator;
class D2Visualization {
    D2Estimator * _estimator = nullptr;
    ros::Publisher odom_pub, imu_prop_pub, pcl_pub;
public:
    void init(ros::NodeHandle & nh, D2Estimator * estimator);
    void postSolve();
};
}