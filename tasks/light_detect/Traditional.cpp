//
// opencv traditional vision detector
// created by cwb on 2026.1.20
//

#include "Traditional.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <iterator>

TraditionalDetector::TraditionalDetector(const std::string &config_path)
    : config_path_(config_path) {
    auto yaml = tools::load(config_path);    
    h_min_ = yaml["hsv"]["h_min"].as<int>();
    h_max_ = yaml["hsv"]["h_max"].as<int>();
    s_min_ = yaml["hsv"]["s_min"].as<int>();
    s_max_ = yaml["hsv"]["s_max"].as<int>() ;
    v_min_ = yaml["hsv"]["v_min"].as<int>();
    v_max_ = yaml["hsv"]["v_max"].as<int>() ;  
    morph_kernel_size_ = yaml["morphology"]["kernel_size"].as<int>();
    morph_iterations_ = yaml["morphology"]["iterations"].as<int>();
    dilate_iterations_ = yaml["morphology"]["dilate_iterations"].as<int>() ;
    erode_iterations_ = yaml["morphology"]["erode_iterations"].as<int>();
    min_area_ = yaml["contour_filter"]["min_area"].as<double>();
    max_area_ = yaml["contour_filter"]["max_area"].as<double>();
    max_aspect_ratio_ = yaml["contour_filter"]["max_aspect_ratio"].as<double>();
    min_aspect_ratio_ = yaml["contour_filter"]["min_aspect_ratio"].as<double>();
    use_roi_ = yaml["use_roi"].as<bool>();
    roi_x_ = yaml["roi"]["x"].as<int>();
    roi_y_ = yaml["roi"]["y"].as<int>();
    roi_width_ = yaml["roi"]["width"].as<int>() ;
    roi_height_ = yaml["roi"]["height"].as<int>() ; 
    gaussian_kernel_size_ = yaml["gaussian"]["kernel_size"].as<int>();
    gaussian_sigma_ = yaml["gaussian"]["sigma"].as<double>();
    use_trackbar_ = yaml["use_trackbar"].as<bool>();
    // Debug
    if (use_trackbar_){
        track_h_min_ = h_min_;
        track_h_max_ = h_max_;
        track_s_min_ = s_min_;
        track_s_max_ = s_max_;
        track_v_min_ = v_min_;
        track_v_max_ = v_max_;
        track_min_area_ = static_cast<int>(min_area_);
        track_max_area_ = static_cast<int>(max_area_);
        track_min_aspect_ratio_ = static_cast<int>(min_aspect_ratio_ * 100);
        track_max_aspect_ratio_ = static_cast<int>(max_aspect_ratio_ * 100);
        track_morph_kernel_size_ = morph_kernel_size_;
        track_dilate_iterations_ = dilate_iterations_;
        track_erode_iterations_ = erode_iterations_;
        track_roi_x_ = roi_x_;
        track_roi_y_ = roi_y_;
        track_roi_width_ = roi_width_;
        track_roi_height_ = roi_height_;
    }
    morph_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(morph_kernel_size_, morph_kernel_size_));
}

std::vector<LightDetect::Light> TraditionalDetector::detect(cv::Mat &src, const cv::Size2d &dst_size, const int &my_color, const bool &startup)
{
    if (src.empty()) return {};
    std::lock_guard<std::mutex> lock(parameter_mtx_);

    cv::Mat resized;
    cv::Size target_size(static_cast<int>(dst_size.width), static_cast<int>(dst_size.height));
    if (src.size() != target_size) {
        cv::resize(src, resized, target_size);
    } else {
        resized = src;
    }
    cv::Size original_size = resized.size();   // = dst_size，mapCoordinates scale=1.0
    cv::Rect roi_rect;
    preprocess(resized, processed_img, roi_rect);
    colorSegmentation(processed_img, bir_img);
    morphologyProcess(bir_img);
    std::vector<Light> lights = findAndFilterContours(bir_img, roi_rect, dst_size, original_size);
    return lights;
}

void TraditionalDetector::preprocess(const cv::Mat &src, cv::Mat &dst,cv::Rect &roi_rect) {
    if (use_roi_) {
        auto x = std::max(0, std::min(roi_x_, src.cols - 1));
        auto y = std::max(0, std::min(roi_y_, src.rows - 1));
        auto w = std::clamp(roi_width_, 1, src.cols - x);
        auto h = std::clamp(roi_height_, 1, src.rows - y);
        roi_rect = cv::Rect(x, y, w, h);
        cv::Mat roi_frame = src(roi_rect);
        debug_roiimg = roi_frame.clone();   // 供可视化显示ROI裁剪图
        cv::GaussianBlur(roi_frame, dst, cv::Size(gaussian_kernel_size_, gaussian_kernel_size_), gaussian_sigma_);
    } else {
        roi_rect = cv::Rect(0, 0, src.cols, src.rows);
        debug_roiimg = src;
        cv::GaussianBlur(src, dst, cv::Size(gaussian_kernel_size_, gaussian_kernel_size_), gaussian_sigma_);
    }
}

void TraditionalDetector::colorSegmentation(const cv::Mat &src,cv::Mat &dst) {
    cv::cvtColor(src, hsv_img, cv::COLOR_BGR2HSV);
    cv::Scalar lower(h_min_, s_min_, v_min_);
    cv::Scalar upper(h_max_, s_max_, v_max_);
    cv::inRange(hsv_img, lower, upper, dst);  
}

void TraditionalDetector::morphologyProcess(cv::Mat &binary) {    
    if (erode_iterations_ > 0) {
        cv::erode(binary, binary, morph_kernel_, cv::Point(-1, -1), erode_iterations_);
    }    
    if (dilate_iterations_ > 0) {
        cv::dilate(binary, binary, morph_kernel_, cv::Point(-1, -1), dilate_iterations_);
    }  
    cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, morph_kernel_, cv::Point(-1, -1), morph_iterations_);    
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, morph_kernel_, cv::Point(-1, -1), 1);
    if(use_trackbar_)this->debug_birimg = binary;
}

std::vector<LightDetect::Light> TraditionalDetector::findAndFilterContours(const cv::Mat &binary, const cv::Rect &roi_offset,const cv::Size2d &dst_size,const cv::Size &original_size) 
{
    std::vector<Light> result;
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(binary, contours, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    for (const auto &contour : contours) {
        double area = cv::contourArea(contour);
        if (area < min_area_ || area > max_area_) continue;
        if (contour.size() < 5) continue;
        
        cv::RotatedRect rRect = cv::fitEllipse(contour);
        float width = rRect.size.width;
        float height = rRect.size.height;    
        float max_axis = std::max(width, height);
        float min_axis = std::min(width, height);
        if (min_axis == 0) continue; 
        
        float aspect_ratio = max_axis / min_axis;       
        if (aspect_ratio > max_aspect_ratio_ || aspect_ratio < min_aspect_ratio_) continue;

        cv::Point2f center;
        float radius;
        cv::minEnclosingCircle(contour, center, radius);

        Light light;
        light.center_point = center;
        light.box = cv::Rect2d(rRect.boundingRect());
        light.min_axis = min_axis;
        light.max_axis = max_axis;
        mapCoordinates(light, roi_offset, dst_size, original_size);
        
        result.push_back(light);
    }
    std::sort(result.begin(), result.end(), [](const Light &a, const Light &b) {
        return a.score > b.score;
    }); 
    return result;
}

void TraditionalDetector::mapCoordinates(Light &light, const cv::Rect &roi_offset,const cv::Size2d &dst_size,const cv::Size &original_size)
{
    (void)roi_offset;

    // 原图坐标 → 目标尺寸坐标（入口缩放后 original_size == dst_size，scale=1.0）
    double scale_x = dst_size.width / original_size.width;
    double scale_y = dst_size.height / original_size.height;

    light.center_point.x *= scale_x;
    light.center_point.y *= scale_y;
    light.box.x *= scale_x;
    light.box.y *= scale_y;
    light.box.width *= scale_x;
    light.box.height *= scale_y;
}

void TraditionalDetector::onTrackbarCallback(int value, void* userdata){
    TraditionalDetector* trad_detector = static_cast<TraditionalDetector*>(userdata);
    trad_detector->updateParameters();
}

void TraditionalDetector::updateParameters(){
    std::lock_guard<std::mutex> lock(parameter_mtx_);
    h_min_ = track_h_min_;
    h_max_ = track_h_max_;
    s_min_ = track_s_min_;
    s_max_ = track_s_max_;
    v_min_ = track_v_min_;
    v_max_ = track_v_max_;   
    min_area_ = static_cast<double>(track_min_area_);
    max_area_ = static_cast<double>(track_max_area_);
    min_aspect_ratio_ = static_cast<double>(track_min_aspect_ratio_) / 100.0;
    max_aspect_ratio_ = static_cast<double>(track_max_aspect_ratio_) / 100.0;
    morph_kernel_size_ = (track_morph_kernel_size_ % 2 == 0) ? 
                         track_morph_kernel_size_ + 1 : track_morph_kernel_size_;
    dilate_iterations_ = track_dilate_iterations_;
    erode_iterations_ = track_erode_iterations_;
    if (use_roi_) {
        roi_x_ = track_roi_x_;
        roi_y_ = track_roi_y_;
        roi_width_ = track_roi_width_;
        roi_height_ = track_roi_height_;
    }
    morph_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(morph_kernel_size_, morph_kernel_size_));
}

void TraditionalDetector::createTrackbars(tools::WebViewer* web_viewer) {
    if (!web_viewer) return;
    web_viewer->setParameterActions([this] { saveParameters(); },
                                  [this] { restoreParameters(); });
    web_viewer->namedWindow("Debug Settings");
    // HSV 
    web_viewer->createTrackbar("H Min", "Debug Settings", &track_h_min_, 180, onTrackbarCallback, this);
    web_viewer->createTrackbar("H Max", "Debug Settings", &track_h_max_, 180, onTrackbarCallback, this);
    web_viewer->createTrackbar("S Min", "Debug Settings", &track_s_min_, 255, onTrackbarCallback, this);
    web_viewer->createTrackbar("S Max", "Debug Settings", &track_s_max_, 255, onTrackbarCallback, this);
    web_viewer->createTrackbar("V Min", "Debug Settings", &track_v_min_, 255, onTrackbarCallback, this);
    web_viewer->createTrackbar("V Max", "Debug Settings", &track_v_max_, 255, onTrackbarCallback, this);
    //形态学
    web_viewer->createTrackbar("Kernel Size", "Debug Settings", &track_morph_kernel_size_, 15, onTrackbarCallback, this);
    web_viewer->createTrackbar("Dilate", "Debug Settings", &track_dilate_iterations_, 10, onTrackbarCallback, this);
    web_viewer->createTrackbar("Erode", "Debug Settings", &track_erode_iterations_, 10, onTrackbarCallback, this);
    // 轮廓筛选
    web_viewer->createTrackbar("Min Area", "Debug Settings", &track_min_area_, 3000, onTrackbarCallback, this);
    web_viewer->createTrackbar("Max Area", "Debug Settings", &track_max_area_, 20000, onTrackbarCallback, this);
    web_viewer->createTrackbar("Min Aspect Ratio", "Debug Settings", &track_min_aspect_ratio_, 200, onTrackbarCallback, this);
    web_viewer->createTrackbar("Max Aspect Ratio", "Debug Settings", &track_max_aspect_ratio_, 200, onTrackbarCallback, this);
    if (use_roi_) {
        // ROI 坐标基于检测入口缩放后的 640x480 图像；超出右/下边界时裁剪到边界。
        web_viewer->createTrackbar("roi.x", "Debug Settings", &track_roi_x_, 639, onTrackbarCallback, this);
        web_viewer->createTrackbar("roi.y", "Debug Settings", &track_roi_y_, 479, onTrackbarCallback, this);
        web_viewer->createTrackbar("roi.width", "Debug Settings", &track_roi_width_, 640, onTrackbarCallback, this, 1);
        web_viewer->createTrackbar("roi.height", "Debug Settings", &track_roi_height_, 480, onTrackbarCallback, this, 1);
    }
}

namespace {
struct ParameterBinding {
    const char* section;
    const char* key;
    int* value;
    int maximum;
    double scale = 1.0;
    int minimum = 0;
};
std::vector<ParameterBinding> parameterBindings(TraditionalDetector& detector, bool use_roi) {
    std::vector<ParameterBinding> bindings{
        {"hsv", "h_min", &detector.track_h_min_, 180}, {"hsv", "h_max", &detector.track_h_max_, 180},
        {"hsv", "s_min", &detector.track_s_min_, 255}, {"hsv", "s_max", &detector.track_s_max_, 255},
        {"hsv", "v_min", &detector.track_v_min_, 255}, {"hsv", "v_max", &detector.track_v_max_, 255},
        {"morphology", "kernel_size", &detector.track_morph_kernel_size_, 15},
        {"morphology", "dilate_iterations", &detector.track_dilate_iterations_, 10},
        {"morphology", "erode_iterations", &detector.track_erode_iterations_, 10},
        {"contour_filter", "min_area", &detector.track_min_area_, 3000},
        {"contour_filter", "max_area", &detector.track_max_area_, 20000},
        {"contour_filter", "min_aspect_ratio", &detector.track_min_aspect_ratio_, 200, 100.0},
        {"contour_filter", "max_aspect_ratio", &detector.track_max_aspect_ratio_, 200, 100.0}
    };
    if (use_roi) {
        bindings.insert(bindings.end(), {
            {"roi", "x", &detector.track_roi_x_, 639},
            {"roi", "y", &detector.track_roi_y_, 479},
            {"roi", "width", &detector.track_roi_width_, 640, 1.0, 1},
            {"roi", "height", &detector.track_roi_height_, 480, 1.0, 1}
        });
    }
    return bindings;
}
}

void TraditionalDetector::saveParameters() {
    const auto bindings = parameterBindings(*this, use_roi_);
    std::vector<tools::NumericYamlParameter> parameters;
    for (const auto& binding : bindings) {
        double value = *binding.value / binding.scale;
        if (std::string(binding.key) == "kernel_size") value = *binding.value | 1;
        parameters.push_back({{binding.section, binding.key}, value});
    }
    tools::save_numeric_parameters(config_path_, parameters);
}

void TraditionalDetector::restoreParameters() {
    const auto yaml = YAML::LoadFile(config_path_);
    const auto bindings = parameterBindings(*this, use_roi_);
    std::vector<int> values(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& binding = bindings[i];
        const double scaled = yaml[binding.section][binding.key].as<double>() * binding.scale;
        if (!std::isfinite(scaled) || scaled < binding.minimum || scaled > binding.maximum ||
            std::abs(scaled - std::round(scaled)) > 1e-6)
            throw std::runtime_error("parameter outside trackbar range");
        values[i] = static_cast<int>(std::round(scaled));
    }
    for (size_t i = 0; i < bindings.size(); ++i) *bindings[i].value = values[i];
    updateParameters();
}
