//
// Created by tony on 22-4-16.
//

#include "MulranPointCloudSelect.h"
#include <iostream>

#define PCL_NO_PRECOMPILE
#include <pcl/point_types.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/impl/passthrough.hpp>

#include <string>
#include <iostream>
#include <sstream>
#include <boost/format.hpp>
#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <vector>
#include <pcl/visualization/pcl_visualizer.h>
#include <Eigen/Core>
#include <pcl/common/transforms.h>

#include <opencv4/opencv2/opencv.hpp>
#include <opencv4/opencv2/core.hpp>

#include <Utility.h>
#include <fmt/core.h>
#include <fmt/format.h>

namespace pcl {
    struct PointXYZIRCT {
        PCL_ADD_POINT4D;
        float intensity;
        std::uint16_t row;
        std::uint16_t col;
        std::uint32_t t;
        std::int16_t label;

        EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    }EIGEN_ALIGN16;
}

POINT_CLOUD_REGISTER_POINT_STRUCT (
    pcl::PointXYZIRCT,
    (float, x, x)
    (float, y, y)
    (float, z, z)
    (float, intensity, intensity)
    (std::uint16_t, row, row)
    (std::uint16_t, col, col)
    (std::uint32_t, t, t)
    (std::int16_t, label, label)
)

float KEYFRAME_DIST_INTERVAL = 2.0f;

//select the dataset source here
std::string dataset_dir_;

std::string ouster_cloud_dir_;
std::string ouster_time_filename_;
std::string gt_pose_filename_;

std::string output_root_dir_;;
std::string output_cloud_dir_;
std::string output_keyframe_pose_data_filename_;
std::string output_keyframe_pose_format_filename_;

std::vector<std::pair<int64_t, Pose6f>> full_gt_poses_;
std::vector<Pose6f> selected_gt_poses_;
std::vector<int64_t> full_cloud_timestamps_;


void initDirectories(std::string root_dir)
{
    if (root_dir.back() != '/') {
        root_dir = root_dir + "/";
    }
    dataset_dir_ = root_dir;

    // input paths
    ouster_cloud_dir_ = dataset_dir_ + "sensor_data/Ouster/";
    ouster_time_filename_ = dataset_dir_ + "sensor_data/ouster_front_stamp.csv";
    gt_pose_filename_ = dataset_dir_ + "global_pose.csv";

    // output paths
    output_root_dir_ = fmt::format("{}selected_keyframes_{:2.2f}m/", dataset_dir_, KEYFRAME_DIST_INTERVAL);
    output_cloud_dir_ = output_root_dir_ + "keyframe_point_cloud/";
    output_keyframe_pose_data_filename_ = output_root_dir_ + "keyframe_pose.csv";
    output_keyframe_pose_format_filename_ = output_root_dir_ + "keyframe_pose_format.csv";
}

pcl::PointCloud<pcl::PointXYZIRCT>::Ptr extractPointCloud(int64_t timestamp)
{
    boost::format fmt("%010ld");
    fmt % timestamp;
    std::string current_file_name = ouster_cloud_dir_ + fmt.str() + ".bin";

    //load current data
    pcl::PointCloud<pcl::PointXYZIRCT>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZIRCT>);

    ifstream file;
    file.open(current_file_name, ios::in|ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open point cloud file: " << current_file_name << std::endl;
        return cloud;
    }

    int k = 0;
    static const int MAX_NUM_POINTS = 64*1024;
    while(!file.eof() && k < MAX_NUM_POINTS)
    {
        pcl::PointXYZIRCT point;
        file.read(reinterpret_cast<char *>(&point.x), sizeof(float));
        file.read(reinterpret_cast<char *>(&point.y), sizeof(float));
        file.read(reinterpret_cast<char *>(&point.z), sizeof(float));
        file.read(reinterpret_cast<char *>(&point.intensity), sizeof(float));
        point.row = static_cast<uint16_t>(k % 64);
        float azimuthal_angle = atan2(point.y, point.x) / M_PI * 180.0f;
        if (azimuthal_angle > 360.0f) {azimuthal_angle = azimuthal_angle - 360.0f;}
        else if (azimuthal_angle < 0.0f) {azimuthal_angle = azimuthal_angle + 360.0f;}
        point.col = static_cast<uint16_t>(round(azimuthal_angle / 360.0f * 1024));
        point.label = -2;
        k ++;
        cloud->push_back(point);
    }
    file.close();

    return cloud;
}

void readFullGtPoses()
{
    std::fstream f_gt_pose;
    f_gt_pose.open(gt_pose_filename_, ios::in);

    if (f_gt_pose.is_open()) {
        std::cout << "loaded gt pose file: " << gt_pose_filename_ << std::endl;
    } else {
        std::cerr << "failed to load gt pose file: " << gt_pose_filename_ << std::endl;
        exit(1);
    }


    std::string tmp_pose_str;
    while (f_gt_pose >> tmp_pose_str) {
        Eigen::Matrix<double,4,4> T = Eigen::Matrix<double,4,4>::Zero();
        T(3,3) = 1.0;

        std::vector<std::string> entry_tokens;

        std::stringstream  ss(tmp_pose_str);
        std::string str;
        while (getline(ss, str, ',')) {
            entry_tokens.push_back(str);
        }
        if(entry_tokens.size() != 13) {
            std::cerr << "size of entry_token is NOT 13. " << std::endl;
            break;
        }
        int64_t stamp_int;
        std::istringstream ( entry_tokens[0] ) >> stamp_int;
        for(int i=0; i<3; i++){
            for(int j=0; j<4; j++){
                double d = std::stod(entry_tokens[1+(4*i)+j]);
                T(i,j) = d;
            }
        }

        //Eigen::Affine3d this_pose(T);
        Eigen::Matrix3d rotation_matrix = T.block<3,3>(0,0);
        Eigen::Quaterniond rotation_quat(rotation_matrix);
        // do NOT use the built-in conversion to euler angles in Eigen
        // Eigen::Vector3d euler_angles = rotation_matrix.eulerAngles(2,1,0); // ZYX order: yaw pitch roll
        Eigen::Vector3d euler_angles = rotationMatrixToEulerAngles(rotation_matrix);
        Pose6f this_pose6f{
                .x = float(T(0,3)),
                .y = float(T(1,3)),
                .z = float(T(2,3)),
                .roll = float(euler_angles(0)),
                .pitch = float(euler_angles(1)),
                .yaw = float(euler_angles(2)),
                .rotation_matrix = rotation_matrix,
                .rotation_quat = rotation_quat
        };

        full_gt_poses_.emplace_back(stamp_int, this_pose6f);
    }

    f_gt_pose.close();

    std::sort(full_gt_poses_.begin(), full_gt_poses_.end(), [](auto &pair_1, auto &pair_2) -> bool {
        return pair_1.first < pair_2.first;
    });

    std::cout << "Finish reading all gt pose, total " << full_gt_poses_.size() << " entries. " << std::endl;
}

void readFullCloudTimestamps()
{
    std::fstream f_cloud_time;
    f_cloud_time.open(ouster_time_filename_, ios::in);


    if(f_cloud_time.is_open()) {
        std::cout << "loaded cloud timestamps: " << ouster_time_filename_ << std::endl;
    } else {
        std::cerr << "failed to load cloud timestamps: " << ouster_time_filename_ << std::endl;
        exit(1);
    }


    std::string tmp_cloud_timestamp_str;
    while (f_cloud_time >> tmp_cloud_timestamp_str) {

        int64_t tmp_cloud_timestamp_i64 = std::stoll(tmp_cloud_timestamp_str);
        full_cloud_timestamps_.emplace_back(tmp_cloud_timestamp_i64);

    }

    f_cloud_time.close();

    std::sort(full_cloud_timestamps_.begin(), full_cloud_timestamps_.end(), [](auto &time_1, auto &time_2) -> bool {
        return time_1 < time_2;
    });

    std::cout << "Finish reading all cloud timestamps, total " << full_cloud_timestamps_.size() << " entries. " << std::endl;
}

float getDistBtwPoses(const Pose6f &pose_1, const Pose6f &pose_2)
{
    float diff_x = pose_2.x - pose_1.x;
    float diff_y = pose_2.y - pose_1.y;
    float diff_z = pose_2.z - pose_1.z;
    return std::sqrt(diff_x * diff_x + diff_y * diff_y + diff_z * diff_z);
}

std::string padString(int keyframe_idx)
{
    boost::format fmt("%06d");
    fmt % keyframe_idx;
    return fmt.str();
}

int main(int argc, char** argv)
{
    // Step 1: Read input arguments and do global var initialization
    if (argv[1] == nullptr) {
    std::string usage_prompt = fmt::format(
"\
Usage: {} [dataset_root_dir] [keyframe_dist_interval](default=2)\n\n \
[dataset_root_dir] should be organized as follows: \n \
[dataset_root_dir]\n \
├ sensor_data/ \n \
│ ├ Ouster/ \n \
│ └ ouster_front_stamp.csv \n \
└ global_pose.csv \n \
\n\n \
This binary selects keyframes based on the interval distance between two consecutive frames. \
You may specify the interval mannualy, e.g. 1 or 2, in meters. \
After running the binary, you will have files organized as follows: \n \
[keyframes_root_dir]\n \
├ ... \n \
└ selected_keyframes_xxm/ \n \
  ├ keyframe_point_cloud/ <- folder for keyframe point clouds in pcd format \n \
  ├ keyframe_pose.csv <- gt poses for all selected keyframes \n \
  └ keyframe_pose_format.csv <- format for one entry in the gt poses \n \
", argv[0]);
        std::cout << usage_prompt << std::endl;
        exit(1);
    }
    
    if (argv[2] != nullptr) {
        KEYFRAME_DIST_INTERVAL = std::stof(std::string(argv[2]));
    }
    std::cout << "Using keyframe_dist_interval = " << KEYFRAME_DIST_INTERVAL << "m. \n";
    initDirectories(std::string(argv[1]));
    std::cout << "Using dataset_dir = " << dataset_dir_ << " \n";

    int unused __attribute__((unused));
    unused = system((std::string("exec rm -r ") + output_root_dir_).c_str());
    unused = system((std::string("mkdir -p ") + output_root_dir_).c_str());

    unused = system((std::string("exec rm -r ") + output_cloud_dir_).c_str());
    unused = system((std::string("mkdir -p ") + output_cloud_dir_).c_str());

    // Step 2: Read GT poses and timestamps
    readFullGtPoses();
    readFullCloudTimestamps();

    // Step 3: Save output pose format descriptions
    std::ofstream f_keyframe_poses_format(output_keyframe_pose_format_filename_, ios::out);
    if (!f_keyframe_poses_format.is_open()) {
        std::cerr << "Failed to create keyframe pose format file: " << output_keyframe_pose_format_filename_ << std::endl;
        exit(1);
    }
    f_keyframe_poses_format << 
            "cloud_idx, x, y, z, roll, pitch, yaw, \
             rotation_matrix(0 0), rotation_matrix(0 1), rotation_matrix(0 2), \
             rotation_matrix(1 0), rotation_matrix(1 1), rotation_matrix(1 2), \
             rotation_matrix(2 0), rotation_matrix(2 1), rotation_matrix(2 2)" 
            << std::endl;
    f_keyframe_poses_format.close();


    // Step 4: Select keyframes and save their interpolated gt poses
    std::ofstream f_keyframe_poses_data(output_keyframe_pose_data_filename_, ios::out);
    if (!f_keyframe_poses_data.is_open()) {
        std::cerr << "Failed to create keyframe pose data file: " << output_keyframe_pose_data_filename_ << std::endl;
        exit(1);
    }

    int keyframe_idx = 0;

    Pose6f last_cloud_pose{.x = 0, .y = 0, .z = 0, .roll = 0, .pitch = 0, .yaw = 0};
    size_t last_gt_pose_idx = 1;
    for (size_t cloud_idx = 0; cloud_idx < full_cloud_timestamps_.size(); cloud_idx ++) {
        int64_t this_cloud_time = full_cloud_timestamps_[cloud_idx];
        int64_t this_pose_time = -1;
        int64_t last_pose_time = -1;
        Pose6f begin_pose, end_pose; // for interpolation
        bool if_found_pose = false;
        for (size_t gt_pose_idx = last_gt_pose_idx; gt_pose_idx < full_gt_poses_.size(); gt_pose_idx ++) {
            this_pose_time = full_gt_poses_[gt_pose_idx].first;
            last_pose_time = full_gt_poses_[gt_pose_idx - 1].first;
            if (last_pose_time <= this_cloud_time && this_pose_time >= this_cloud_time) {
                last_gt_pose_idx = gt_pose_idx;
                if_found_pose = true;
                begin_pose = full_gt_poses_[gt_pose_idx - 1].second;
                end_pose = full_gt_poses_[gt_pose_idx].second;
                break;
            }
        }

        if (this_pose_time == -1 || last_pose_time == -1 || !if_found_pose) {
            std::cerr << "Could not find pose for cloud at timestamp: " << this_cloud_time << std::endl;
            continue;
        }

        double lamda = double(this_cloud_time - last_pose_time) / double(this_pose_time - last_pose_time);
        //Pose6f cloud_pose = begin_pose * (1 - lamda) + end_pose * lamda;
        //when lamda equals 1.0, cloud_pose is equal to the end_pose
        Pose6f cloud_pose = begin_pose.interpolate(end_pose, lamda);

        float dist_to_last_keyframe = getDistBtwPoses(cloud_pose, last_cloud_pose);
        if (dist_to_last_keyframe < KEYFRAME_DIST_INTERVAL) {
            //NOT a keyframe
            continue;
        }

        // save keyframe cloud and its pose to local
        std::cout << "Saving keyframe: " << keyframe_idx << ", dist to last keyframe: " << dist_to_last_keyframe << std::endl;
        pcl::PointCloud<pcl::PointXYZIRCT>::Ptr this_cloud = extractPointCloud(this_cloud_time);
        pcl::io::savePCDFileBinary(output_cloud_dir_ + padString(keyframe_idx) + ".pcd", *this_cloud);
        std::string pose_entry = 
        fmt::format("{:06d},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f}\n",
            cloud_idx, cloud_pose.x, cloud_pose.y, cloud_pose.z, cloud_pose.roll, cloud_pose.pitch, cloud_pose.yaw, 
            cloud_pose.rotation_matrix(0,0), cloud_pose.rotation_matrix(0,1), cloud_pose.rotation_matrix(0,2), 
            cloud_pose.rotation_matrix(1,0), cloud_pose.rotation_matrix(1,1), cloud_pose.rotation_matrix(1,2), 
            cloud_pose.rotation_matrix(2,0), cloud_pose.rotation_matrix(2,1), cloud_pose.rotation_matrix(2,2));
        f_keyframe_poses_data << pose_entry;
        selected_gt_poses_.push_back(cloud_pose);

        keyframe_idx ++;
        last_cloud_pose = cloud_pose;

    }

    f_keyframe_poses_data.close();

    std::cout << "Done. " << std::endl;

    return 0;
}