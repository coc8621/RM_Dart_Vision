#ifndef IO__CAMERA_HPP
#define IO__CAMERA_HPP

#include <chrono>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <stdexcept>

namespace io
{
struct CameraParameter {
  double value = 0, minimum = 0, maximum = 1, step = 0.01;
  bool enabled = false;
  std::string reason = "相机尚未连接";
  std::string unit;
};

class CameraBase
{
public:
  virtual ~CameraBase() = default;
  virtual void read(cv::Mat & img, std::chrono::steady_clock::time_point & timestamp) = 0;
  virtual CameraParameter parameter(const std::string & name) = 0;
  virtual void setParameter(const std::string & name, double value) = 0;
};

class Camera
{
public:
  Camera(const std::string & config_path);
  void read(cv::Mat & img, std::chrono::steady_clock::time_point & timestamp);
  CameraParameter parameter(const std::string & name) { return camera_->parameter(name); }
  void setParameter(const std::string & name, double value) { camera_->setParameter(name, value); }

private:
  std::unique_ptr<CameraBase> camera_;
};

}  // namespace io

#endif  // IO__CAMERA_HPP