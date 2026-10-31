#include "VisionPipeline.hpp"
#include "tools/logger.hpp"
#include <iostream>
#include <string>
#include <cmath>

VisionPipeline::VisionPipeline(const std::string& config_path)
    : tracker_(config_path), config_path_(config_path)
{
    auto yaml = tools::load(config_path);
    configured_exposure_ms_ = yaml["exposure_ms"].as<double>();
    configured_gain_ = yaml["gain"].as<double>(0);
    auto use_serial = yaml["use_serial"].as<bool>();
    auto record_fps = yaml["record_fps"].as<double>();
    pixel_compensation_ = yaml["pixel_compensation"].as<double>();
    debug_ = yaml["debug"].as<bool>();
    use_trackbar_ = yaml["use_trackbar"].as<bool>();
    detector_type_ = yaml["detector"].as<std::string>();
    record_ = yaml["record"].as<bool>();
    use_roi_ = yaml["use_roi"].as<bool>();

    if (use_serial) {
        gimbal_ = std::make_unique<io::Gimbal>(config_path);
    }

    image_source_ = io::get_image_source(config_path);
    detector_ = LightDetect::load_detector(config_path);

    if (record_) {
        tools::logger()->info("open record mode");
        recorder_ = std::make_unique<tools::Recorder>(record_fps);
    }
}

void VisionPipeline::setupCameraTrackbars(tools::WebViewer& viewer)
{
    if (!debug_ || !use_trackbar_) return;
    auto* camera = image_source_ ? image_source_->camera() : nullptr;
    for (const std::string name : {"exposure_ms", "gain"}) {
        viewer.createDynamicTrackbar(name, "Camera Settings", [this, camera, name] {
            io::CameraParameter info;
            if (camera) info = camera->parameter(name);
            else {
                info.value = name == "exposure_ms" ? configured_exposure_ms_ : configured_gain_;
                info.maximum = std::max(1.0, info.value);
                info.reason = "视频回放不支持调整相机曝光和增益";
                info.unit = name == "exposure_ms" ? "ms" : "";
            }
            if (name == "exposure_ms") {
                info.minimum = 0.0;
                info.maximum = 15.0;
                info.step = 0.05;
            }
            return tools::WebViewer::SliderState{info.value, info.minimum, info.maximum,
                                                 info.step, info.enabled, info.reason, info.unit};
        }, [camera, name](double value) {
            if (!camera) throw std::runtime_error("当前图像来源不是相机");
            camera->setParameter(name, value);
        });
    }
    if (!camera) return;
    viewer.addParameterActions([this, camera] {
        const auto exposure = camera->parameter("exposure_ms");
        const auto gain = camera->parameter("gain");
        if (!exposure.enabled || !gain.enabled) throw std::runtime_error("相机尚未就绪");
        tools::save_numeric_parameters(config_path_, {
            {{"exposure_ms"}, exposure.value}, {{"gain"}, gain.value}
        });
    }, [this, camera] {
        const auto yaml = YAML::LoadFile(config_path_);
        const double exposure = yaml["exposure_ms"].as<double>();
        const double gain = yaml["gain"].as<double>();
        const auto old_exposure = camera->parameter("exposure_ms");
        const auto old_gain = camera->parameter("gain");
        if (!old_exposure.enabled || !old_gain.enabled) throw std::runtime_error("相机尚未就绪");
        if (!std::isfinite(exposure) || !std::isfinite(gain) ||
            exposure < old_exposure.minimum || exposure > old_exposure.maximum ||
            gain < old_gain.minimum || gain > old_gain.maximum)
            throw std::runtime_error("YAML 中的曝光或增益超出相机支持范围");
        camera->setParameter("exposure_ms", exposure);
        try { camera->setParameter("gain", gain); }
        catch (...) {
            camera->setParameter("exposure_ms", old_exposure.value);
            throw;
        }
    });
}

bool VisionPipeline::step(FrameResult& out)
{
    if (!detector_) return false;

    image_source_->read(out.frame, out.timestamp);
    if (out.frame.empty()) {
        std::cout << "Empty frame!" << std::endl;
        return true;   // 空帧不退出，继续下一轮
    }

    out.lights = detector_->detect(out.frame, cv::Size2d(640, 480), 0, is_startup_);
    is_startup_ = false;

    //   不开ROI时为640x480缩放图，开ROI时为ROI裁剪图
    //   不是tradition检测器用原图
    out.display_frame = out.frame;
    if (detector_type_ == "tradition") {
        if (auto* td = tradDetector(); td && !td->debug_roiimg.empty()) {
            out.display_frame = td->debug_roiimg;
        }
    }

    out.target = trackAndDispatch(out.lights, out.timestamp, out.pixel_offset);
    return true;
}

void VisionPipeline::record(const cv::Mat& img, const std::chrono::steady_clock::time_point& ts)
{
    if (recorder_) {
        recorder_->record(img, Eigen::Quaterniond::Identity(), ts);
    }
}

GreenLightTracker::TrackedTarget VisionPipeline::trackAndDispatch(
    const std::vector<LightDetect::Light>& lights,
    const std::chrono::steady_clock::time_point& ts,
    float& out_offset)
{
    auto tgt = tracker_.track(lights, ts);
    out_offset = 0.0f;

    if (tgt.is_tracking) {
        out_offset = tgt.center_point.x - kHalfWidth_ + pixel_compensation_;
        if (gimbal_) {
            io::VisionToGimbal send_data;
            send_data.yaw_offset = out_offset;
            gimbal_->send(send_data);
        }
        tools::logger()->debug("Pixel Offset: {:.2f} (state={})",
                               out_offset,
                               debug_ ? GreenLightTracker::stateStr(tgt.state)
                                      : std::to_string(static_cast<int>(tgt.state)));
    } else {
        tools::logger()->debug("Tracking not active, state={}",
                               debug_ ? GreenLightTracker::stateStr(tgt.state)
                                      : std::to_string(static_cast<int>(tgt.state)));
    }
    return tgt;
}
