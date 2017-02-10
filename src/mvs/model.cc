// COLMAP - Structure-from-Motion and Multi-View Stereo.
// Copyright (C) 2016  Johannes L. Schoenberger <jsch at inf.ethz.ch>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "mvs/model.h"

#include "base/pose.h"
#include "base/reconstruction.h"
#include "util/alignment.h"
#include "util/logging.h"
#include "util/misc.h"
#include "util/string.h"

namespace colmap {
namespace mvs {

void Model::Read(const std::string& path, const std::string& format) {
  if (format == "COLMAP") {
    ReadFromCOLMAP(path);
  } else if (format == "PMVS") {
    ReadFromPMVS(path);
  } else {
    LOG(FATAL) << "Invalid input format";
  }
}

void Model::ReadFromCOLMAP(const std::string& path) {
  Reconstruction reconstruction;
  reconstruction.Read(JoinPaths(path, "sparse"));

  images.reserve(reconstruction.NumRegImages());
  std::unordered_map<image_t, size_t> image_id_map;
  for (size_t i = 0; i < reconstruction.NumRegImages(); ++i) {
    const auto image_id = reconstruction.RegImageIds()[i];
    const auto& image = reconstruction.Image(image_id);
    const auto& camera = reconstruction.Camera(image.CameraId());

    CHECK_EQ(camera.ModelId(), PinholeCameraModel::model_id);

    const std::string image_path = JoinPaths(path, "images", image.Name());
    const Eigen::Matrix<float, 3, 3, Eigen::RowMajor> K =
        camera.CalibrationMatrix().cast<float>();
    const Eigen::Matrix<float, 3, 3, Eigen::RowMajor> R =
        QuaternionToRotationMatrix(image.Qvec()).cast<float>();
    const Eigen::Vector3f T = image.Tvec().cast<float>();

    images.emplace_back(image_path, K.data(), R.data(), T.data());
    image_id_map.emplace(image_id, i);
    image_names_.push_back(image.Name());
    image_name_to_id_.emplace(image.Name(), i);
  }

  points.reserve(reconstruction.NumPoints3D());
  for (const auto& point3D : reconstruction.Points3D()) {
    Point point;
    point.x = point3D.second.X();
    point.y = point3D.second.Y();
    point.z = point3D.second.Z();
    point.track.reserve(point3D.second.Track().Length());
    for (const auto& track_el : point3D.second.Track().Elements()) {
      point.track.push_back(image_id_map.at(track_el.image_id));
    }
    points.push_back(point);
  }

  depth_maps.resize(images.size());
  normal_maps.resize(images.size());
  consistency_graph.resize(images.size());
}

void Model::ReadFromPMVS(const std::string& path) {
  const std::string bundle_file_path = JoinPaths(path, "bundle.rd.out");

  std::ifstream file(bundle_file_path);
  CHECK(file.is_open()) << bundle_file_path;

  // Header line.
  std::string header;
  std::getline(file, header);

  int num_images, num_points;
  file >> num_images >> num_points;

  images.reserve(num_images);
  for (int image_id = 0; image_id < num_images; ++image_id) {
    const std::string image_name = StringPrintf("%08d.jpg", image_id);
    const std::string image_path = JoinPaths(path, "visualize", image_name);

    float K[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    file >> K[0];
    K[4] = K[0];

    Bitmap bitmap;
    CHECK(bitmap.Read(image_path));
    K[2] = bitmap.Width() / 2.0f;
    K[5] = bitmap.Height() / 2.0f;

    float k1, k2;
    file >> k1 >> k2;
    CHECK_EQ(k1, 0.0f);
    CHECK_EQ(k2, 0.0f);

    float R[9];
    for (size_t i = 0; i < 9; ++i) {
      file >> R[i];
    }
    for (size_t i = 3; i < 9; ++i) {
      R[i] = -R[i];
    }

    float T[3];
    file >> T[0] >> T[1] >> T[2];
    T[1] = -T[1];
    T[2] = -T[2];

    images.emplace_back(image_path, K, R, T);
    image_names_.push_back(image_name);
    image_name_to_id_.emplace(image_name, image_id);
  }

  points.resize(num_points);
  for (int point_id = 0; point_id < num_points; ++point_id) {
    auto& point = points[point_id];

    file >> point.x >> point.y >> point.z;

    int color[3];
    file >> color[0] >> color[1] >> color[2];

    int track_len;
    file >> track_len;
    point.track.resize(track_len);

    for (int i = 0; i < track_len; ++i) {
      int feature_idx;
      float imx, imy;
      file >> point.track[i] >> feature_idx >> imx >> imy;
      CHECK_LT(point.track[i], images.size());
    }
  }

  depth_maps.resize(images.size());
  normal_maps.resize(images.size());
  consistency_graph.resize(images.size());
}

int Model::GetImageId(const std::string& name) const {
  CHECK_GT(image_name_to_id_.count(name), 0) << "Image with name `" << name
                                             << "` does not exist";
  return image_name_to_id_.at(name);
}

std::string Model::GetImageName(const int image_id) const {
  CHECK_GE(image_id, 0);
  CHECK_LT(image_id, image_names_.size());
  return image_names_.at(image_id);
}

std::vector<std::pair<float, float>> Model::ComputeDepthRanges() const {
  std::vector<std::vector<float>> depths(images.size());
  for (const auto& point : points) {
    const Eigen::Vector3f X(point.x, point.y, point.z);
    for (const auto& image_id : point.track) {
      const auto& image = images.at(image_id);
      const float depth =
          Eigen::Map<const Eigen::Vector3f>(&image.GetR()[6]).dot(X) +
          image.GetT()[2];
      if (depth > 0) {
        depths[image_id].push_back(depth);
      }
    }
  }

  std::vector<std::pair<float, float>> depth_ranges(depths.size());
  for (size_t image_id = 0; image_id < depth_ranges.size(); ++image_id) {
    auto& depth_range = depth_ranges[image_id];

    auto& image_depths = depths[image_id];

    if (image_depths.empty()) {
      depth_range.first = -1.0f;
      depth_range.second = -1.0f;
      continue;
    }

    std::sort(image_depths.begin(), image_depths.end());

    const float kMinPercentile = 0.01f;
    const float kMaxPercentile = 0.99f;
    depth_range.first = image_depths[image_depths.size() * kMinPercentile];
    depth_range.second = image_depths[image_depths.size() * kMaxPercentile];

    const float kStretchRatio = 0.25f;
    depth_range.first *= (1.0f - kStretchRatio);
    depth_range.second *= (1.0f + kStretchRatio);
  }

  return depth_ranges;
}

std::vector<std::map<int, int>> Model::ComputeSharedPoints() const {
  std::vector<std::map<int, int>> shared_points(images.size());
  for (const auto& point : points) {
    for (size_t i = 0; i < point.track.size(); ++i) {
      const int image_id1 = point.track[i];
      for (size_t j = 0; j < i; ++j) {
        const int image_id2 = point.track[j];
        if (image_id1 != image_id2) {
          shared_points.at(image_id1)[image_id2] += 1;
          shared_points.at(image_id2)[image_id1] += 1;
        }
      }
    }
  }
  return shared_points;
}

}  // namespace mvs
}  // namespace colmap
