#ifndef IO__MINDVISION_HPP
#define IO__MINDVISION_HPP

#include <chrono>
#include <opencv2/opencv.hpp>
#include <thread>
#include <atomic>
#include <mutex>

#include "CameraApi.h"
#include "io/camera.hpp"
#include "tools/thread_safe_queue.hpp"

namespace io
{
class MindVision : public CameraBase
{
public:
  MindVision(double exposure_ms, double gamma, const std::string & vid_pid, double gain = -1);
  ~MindVision() override;
  void read(cv::Mat & img, std::chrono::steady_clock::time_point & timestamp) override;
  CameraParameter parameter(const std::string & name) override;
  void setParameter(const std::string & name, double value) override;

private:
  struct CameraData
  {
    cv::Mat img;
    std::chrono::steady_clock::time_point timestamp;
  };

  double exposure_ms_, gamma_;
  double gain_;
  std::mutex settings_mutex_;
  bool settings_ready_ = false;
  CameraParameter parameter_unlocked(const std::string & name);
  CameraHandle handle_;
  int height_, width_;
  std::atomic<bool> quit_, ok_;
  std::thread capture_thread_;
  std::thread daemon_thread_;
  tools::ThreadSafeQueue<CameraData> queue_;
  int vid_, pid_;

  void open();
  void try_open();
  void close();
  void set_vid_pid(const std::string & vid_pid);
  void reset_usb() const;
};

}  // namespace io

#endif  // IO__MINDVISION_HPP