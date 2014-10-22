/*
Copyright (c) 2010-2014, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <ros/ros.h>
#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>

#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/CameraInfo.h>
#include <stereo_msgs/DisparityImage.h>

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>

#include <image_geometry/pinhole_camera_model.h>

#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/subscriber.h>

#include <cv_bridge/cv_bridge.h>
#include <opencv2/highgui/highgui.hpp>

#include "rtabmap/core/util3d.h"

namespace rtabmap
{

class PointCloudXYZ : public nodelet::Nodelet
{
public:
	PointCloudXYZ() : voxelSize_(0.0), decimation_(1) {}

	virtual ~PointCloudXYZ()
	{
		delete sync_;
		delete syncDisparity_;
	}

private:
	virtual void onInit()
	{
		ros::NodeHandle & nh = getNodeHandle();
		ros::NodeHandle & pnh = getPrivateNodeHandle();

		int queueSize = 10;
		pnh.param("queue_size", queueSize, queueSize);
		pnh.param("voxel_size", voxelSize_, voxelSize_);
		pnh.param("decimation", decimation_, decimation_);

		sync_ = new message_filters::Synchronizer<MySyncPolicy>(MySyncPolicy(queueSize), imageDepthSub_, cameraInfoSub_);
		sync_->registerCallback(boost::bind(&PointCloudXYZ::callback, this, _1, _2));

		syncDisparity_ = new message_filters::Synchronizer<MySyncDispPolicy>(MySyncDispPolicy(queueSize), disparitySub_, disparityCameraInfoSub_);
		syncDisparity_->registerCallback(boost::bind(&PointCloudXYZ::callbackDisparity, this, _1, _2));

		image_transport::ImageTransport it(nh);
		imageDepthSub_.subscribe(it, "depth/image", 1);
		cameraInfoSub_.subscribe(nh, "depth/camera_info", 1);

		disparitySub_.subscribe(nh, "disparity/image", 1);
		disparityCameraInfoSub_.subscribe(nh, "disparity/camera_info", 1);

		cloudPub_ = nh.advertise<sensor_msgs::PointCloud2>("cloud", 1);
	}



	void callback(
			  const sensor_msgs::ImageConstPtr& depth,
			  const sensor_msgs::CameraInfoConstPtr& cameraInfo)
	{
		if(depth->encoding.compare(sensor_msgs::image_encodings::TYPE_16UC1)!=0 &&
		   depth->encoding.compare(sensor_msgs::image_encodings::TYPE_32FC1)!=0)
		{
			ROS_ERROR("Input type depth=32FC1,16UC1");
			return;
		}

		if(cloudPub_.getNumSubscribers())
		{
			cv_bridge::CvImageConstPtr imageDepthPtr = cv_bridge::toCvShare(depth);

			image_geometry::PinholeCameraModel model;
			model.fromCameraInfo(*cameraInfo);
			float fx = model.fx();
			float fy = model.fy();
			float cx = model.cx();
			float cy = model.cy();

			pcl::PointCloud<pcl::PointXYZ>::Ptr pclCloud;
			pclCloud = rtabmap::util3d::cloudFromDepth(
					imageDepthPtr->image,
					cx,
					cy,
					fx,
					fy,
					decimation_);

			if(voxelSize_ > 0.0)
			{
				pclCloud = rtabmap::util3d::voxelize(pclCloud, voxelSize_);
			}

			//*********************
			// Publish Map
			//*********************

			sensor_msgs::PointCloud2 rosCloud;
			pcl::toROSMsg(*pclCloud, rosCloud);
			rosCloud.header.stamp = depth->header.stamp;
			rosCloud.header.frame_id = depth->header.frame_id;

			//publish the message
			cloudPub_.publish(rosCloud);
		}
	}

	void callbackDisparity(
			const stereo_msgs::DisparityImageConstPtr& disparityMsg,
			const sensor_msgs::CameraInfoConstPtr& cameraInfo)
	{
		if(disparityMsg->image.encoding.compare(sensor_msgs::image_encodings::TYPE_32FC1) !=0)
		{
			ROS_ERROR("Input type must be disparity=32FC1");
			return;
		}

		// sensor_msgs::image_encodings::TYPE_32FC1
		cv::Mat disparity(disparityMsg->image.height, disparityMsg->image.width, CV_32FC1, const_cast<uchar*>(disparityMsg->image.data.data()));

		if(cloudPub_.getNumSubscribers())
		{
			image_geometry::PinholeCameraModel model;
			model.fromCameraInfo(*cameraInfo);
			float cx = model.cx();
			float cy = model.cy();

			pcl::PointCloud<pcl::PointXYZ>::Ptr pclCloud;
			pclCloud = rtabmap::util3d::cloudFromDisparity(
					disparity,
					cx,
					cy,
					disparityMsg->f,
					disparityMsg->T,
					decimation_);

			if(voxelSize_ > 0.0)
			{
				pclCloud = rtabmap::util3d::voxelize(pclCloud, voxelSize_);
			}

			//*********************
			// Publish Map
			//*********************

			sensor_msgs::PointCloud2 rosCloud;
			pcl::toROSMsg(*pclCloud, rosCloud);
			rosCloud.header.stamp = disparityMsg->header.stamp;
			rosCloud.header.frame_id = disparityMsg->header.frame_id;

			//publish the message
			cloudPub_.publish(rosCloud);
		}
	}

private:

	double voxelSize_;
	int decimation_;
	ros::Publisher cloudPub_;

	image_transport::SubscriberFilter imageDepthSub_;
	message_filters::Subscriber<sensor_msgs::CameraInfo> cameraInfoSub_;

	message_filters::Subscriber<stereo_msgs::DisparityImage> disparitySub_;
	message_filters::Subscriber<sensor_msgs::CameraInfo> disparityCameraInfoSub_;

	typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::CameraInfo> MySyncPolicy;
	message_filters::Synchronizer<MySyncPolicy> * sync_;

	typedef message_filters::sync_policies::ApproximateTime<stereo_msgs::DisparityImage, sensor_msgs::CameraInfo> MySyncDispPolicy;
	message_filters::Synchronizer<MySyncDispPolicy> * syncDisparity_;
};

PLUGINLIB_EXPORT_CLASS(rtabmap::PointCloudXYZ, nodelet::Nodelet);
}

