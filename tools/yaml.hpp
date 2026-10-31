#ifndef TOOLS__YAML_HPP
#define TOOLS__YAML_HPP

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "tools/logger.hpp"

namespace tools
{
inline YAML::Node load(const std::string & path)
{
  try {
    return YAML::LoadFile(path);
  } catch (const YAML::BadFile & e) {
    logger()->error("[YAML] Failed to load file: {}", e.what());
    exit(1);
  } catch (const YAML::ParserException & e) {
    logger()->error("[YAML] Parser error: {}", e.what());
    exit(1);
  }
}

struct NumericYamlParameter {
    std::vector<std::string> keys;
    double value;
};

// 按标量位置更新数值，保留原 YAML 注释和无关配置；完成后原子替换。
inline void save_numeric_parameters(const std::string& path,
                                    const std::vector<NumericYamlParameter>& parameters)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open config");
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto yaml = YAML::Load(text);
    struct Edit { size_t start; size_t length; std::string value; };
    std::vector<Edit> edits;
    for (const auto& parameter : parameters) {
        YAML::Node node = yaml;
        // reset 重新绑定节点，避免 YAML::Node 赋值修改原节点。
        for (const auto& key : parameter.keys) node.reset(node[key]);
        // 只替换数值标量，保留配置的注释、排版及其他配置项。
        node.as<double>();
        const auto mark = node.Mark();
        if (mark.pos < 0 || static_cast<size_t>(mark.pos) >= text.size())
            throw std::runtime_error("invalid scalar position");
        const size_t start = static_cast<size_t>(mark.pos);
        size_t end = start;
        if (text[start] == '\'' || text[start] == '"') {
            end = text.find(text[start], start + 1);
            if (end == std::string::npos) throw std::runtime_error("invalid quoted scalar");
            ++end;
        } else {
            if (std::string("0123456789-+.").find(text[start]) == std::string::npos)
                throw std::runtime_error("parameter must be a numeric scalar");
            end = text.find_first_of(" \t\r\n#,]}", start);
            if (end == std::string::npos) end = text.size();
        }
        YAML::Emitter scalar;
        scalar << parameter.value;
        edits.push_back({start, end - start, scalar.c_str()});
    }
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.start > b.start; });
    for (const auto& edit : edits) text.replace(edit.start, edit.length, edit.value);
    // 写完临时文件再替换，失败时保留原配置。
    const auto temp = path + ".tmp";
    try {
        std::ofstream output;
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.open(temp, std::ios::trunc);
        output << text;
        output.close();
        if (std::rename(temp.c_str(), path.c_str()) != 0)
            throw std::runtime_error("cannot replace config");
    } catch (...) {
        std::remove(temp.c_str());
        throw;
    }
}

template <typename T>
inline T read(const YAML::Node & yaml, const std::string & key)
{
  if (yaml[key]) return yaml[key].as<T>();
  logger()->error("[YAML] {} not found!", key);
  exit(1);
}

}  // namespace tools

#endif  // TOOLS__YAML_HPP