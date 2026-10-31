#include "web_viewer.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include "tools/logger.hpp"

namespace tools
{

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static std::string url_decode(const std::string& str) {
    std::string out;
    out.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int val = 0;
            std::istringstream iss(str.substr(i + 1, 2));
            iss >> std::hex >> val;
            out += static_cast<char>(val);
            i += 2;
        } else if (str[i] == '+') {
            out += ' ';
        } else {
            out += str[i];
        }
    }
    return out;
}

static std::string query_param(const std::string& path,
                               const std::string& param) {
    auto qpos = path.find('?');
    if (qpos == std::string::npos) return "";
    std::string query = path.substr(qpos + 1);
    size_t pos = 0;
    while (pos < query.size()) {
        auto eq  = query.find('=', pos);
        auto amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        if (eq != std::string::npos && eq < amp) {
            if (query.substr(pos, eq - pos) == param)
                return url_decode(query.substr(eq + 1, amp - eq - 1));
        }
        pos = amp + 1;
    }
    return "";
}

// ---------------------------------------------------------------------------
// HTML page (embedded)
// ---------------------------------------------------------------------------

static const char* HTML_PAGE = R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dart WebViewer</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
[hidden]{display:none!important}
body{background:#11111b;color:#cdd6f4;font-family:system-ui,-apple-system,sans-serif;
     min-height:100vh;min-height:100dvh;display:flex;flex-direction:column}
header,footer{background:#181825;padding:14px 20px;display:flex;align-items:center;
              justify-content:space-between;gap:12px;flex-wrap:wrap;border-bottom:1px solid #313244}
header h1{font-size:18px;font-weight:600}
header .dot{width:10px;height:10px;border-radius:50%;background:#a6e3a1;display:inline-block;margin-right:8px}
header .dot.off{background:#f38ba8}
#hdr-info{font-size:12px;color:#a6adc8;overflow-wrap:anywhere;min-width:0}
.layout{display:grid;grid-template-columns:minmax(0,1fr);gap:16px;padding:16px;flex:1;align-items:start}
.layout.has-controls{grid-template-columns:minmax(0,1fr) minmax(280px,340px)}
main{display:grid;gap:16px;min-width:0}
.card{background:#313244;border-radius:10px;overflow:hidden;min-width:0;box-shadow:0 4px 12px #0003}
.card .title{padding:10px 14px;background:#45475a;font-size:14px;overflow-wrap:anywhere}
.card img{display:block;width:100%;height:auto;object-fit:contain;background:#000}
.controls{background:#1e1e2e;border:1px solid #313244;border-radius:10px;padding:16px;min-width:0}
.controls h2{font-size:16px;margin-bottom:12px}
.parameter-tabs{display:flex;gap:8px;margin-bottom:14px}
.parameter-tabs button{background:transparent;color:#cdd6f4;border-color:#45475a}
.parameter-tabs button[aria-selected="true"]{background:#89b4fa;color:#11111b;border-color:#89b4fa}
.actions{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:10px}
button{flex:1;padding:11px 8px;min-height:44px;border:1px solid #89b4fa;border-radius:6px;
       background:#89b4fa;color:#11111b;font:inherit;font-size:14px;cursor:pointer}
button.secondary{background:transparent;color:#cdd6f4}
button:disabled{opacity:.5;cursor:wait}
button:focus-visible,input:focus-visible{outline:2px solid #f9e2af;outline-offset:3px}
.hint{font-size:12px;line-height:1.6;color:#a6adc8;margin-bottom:14px}
.trackbar{background:#313244;padding:10px 12px;border-radius:6px;display:grid;
          grid-template-columns:minmax(0,1fr) auto;gap:4px 12px;margin-top:8px;min-width:0}
.trackbar label{font-size:13px;overflow-wrap:anywhere}
.trackbar input{grid-column:1/-1;width:100%;min-width:0;min-height:32px;accent-color:#89b4fa}
.trackbar .val{font-family:monospace;font-size:13px;color:#a6e3a1}
.parameter-note{grid-column:1/-1;font-size:12px;color:#a6adc8}
.empty{color:#a6adc8;font-size:15px;padding:40px 12px;text-align:center}
footer{font-size:12px;color:#a6adc8;border-top:1px solid #313244;padding-bottom:max(12px,env(safe-area-inset-bottom))}
.toast{position:fixed;bottom:max(24px,env(safe-area-inset-bottom));left:50%;transform:translateX(-50%);
       max-width:calc(100% - 24px);background:#b4befe;color:#1e1e2e;padding:10px 18px;border-radius:12px;
       font-size:13px;opacity:0;transition:opacity .2s;pointer-events:none;z-index:9999}
.toast.show{opacity:1}
@media(min-width:701px){
  .card img{max-height:max(180px,calc((100dvh - 160px)/2))}
  .controls{position:sticky;top:12px;max-height:calc(100dvh - 24px);overflow-y:auto}
}
@media(max-width:700px){
  .layout,.layout.has-controls{grid-template-columns:minmax(0,1fr);padding:10px;gap:12px}
  header,footer{padding:12px}main{gap:12px}.controls{padding:12px}
}
</style>
</head>
<body tabindex="0">
<header>
  <h1><span class="dot" id="dot"></span>Dart WebViewer</h1>
  <span id="hdr-info" style="font-size:12px;color:#6c7086"></span>
</header>
<div class="layout" id="layout">
<main id="container">
  <div class="empty" id="placeholder">等待视频流连接 ...</div>
</main>
<aside id="controls" class="controls" hidden aria-label="调试参数">
  <h2>调试参数</h2>
  <div class="parameter-tabs" role="tablist" aria-label="参数页面">
    <button id="tab-opencv" type="button" role="tab" aria-selected="true"
            aria-controls="panel-opencv">OpenCV 参数</button>
    <button id="tab-camera" type="button" role="tab" aria-selected="false"
            aria-controls="panel-camera" tabindex="-1">相机参数</button>
  </div>
  <div class="actions">
    <button id="save-params" type="button">保存参数</button>
    <button id="restore-params" type="button" class="secondary">恢复之前参数</button>
  </div>
  <p class="hint">保存和恢复对两个页面的全部参数生效，使用当前YAML配置文件。</p>
  <section id="panel-opencv" role="tabpanel" aria-labelledby="tab-opencv">
    <div id="trackbars-opencv"></div>
    <p id="empty-opencv" class="hint" hidden>当前未启用 OpenCV 调参。</p>
  </section>
  <section id="panel-camera" role="tabpanel" aria-labelledby="tab-camera" hidden>
    <div id="trackbars-camera"></div>
    <p id="empty-camera" class="hint" hidden>当前没有相机参数。</p>
  </section>
</aside>
</div>
<footer>
  <span id="status">初始化中...</span>
  <span>点击页面后可使用键盘控制</span>
</footer>
<div class="toast" id="toast" role="status" aria-live="polite"></div>
<script>
const known = new Map();
const controls = new Map();
let toastTimer = null;
let pendingUpdates = Promise.resolve();
let updateFailed = false;
let parameterAction = false;
function selectParameterPage(page) {
  ['opencv', 'camera'].forEach(name => {
    const selected = name === page;
    const tab = document.getElementById('tab-' + name);
    tab.setAttribute('aria-selected', String(selected));
    tab.tabIndex = selected ? 0 : -1;
    document.getElementById('panel-' + name).hidden = !selected;
  });
}
['opencv', 'camera'].forEach(name => {
  const tab = document.getElementById('tab-' + name);
  tab.addEventListener('click', () => selectParameterPage(name));
  tab.addEventListener('keydown', e => {
    let page;
    if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') page = name === 'opencv' ? 'camera' : 'opencv';
    else if (e.key === 'Home') page = 'opencv';
    else if (e.key === 'End') page = 'camera';
    else return;
    e.preventDefault();
    selectParameterPage(page);
    document.getElementById('tab-' + page).focus();
  });
});
function showToast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), 3000);
}
async function post(path, body) {
  const r = await fetch(path, {method:'POST', headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body || {})});
  if (!r.ok) throw new Error(await r.text());
}
document.body.addEventListener('keydown', e => {
  if (e.target.closest('input,button,textarea,select') || e.ctrlKey || e.metaKey || e.altKey) return;
  let code;
  if (e.key === 'Escape') code = 27;
  else if (e.key === 'Enter') code = 13;
  else if (e.key === 'Backspace') code = 8;
  else if (e.key.length === 1) code = e.key.charCodeAt(0);
  else return;
  e.preventDefault();
  showToast('按键: ' + (code === 27 ? 'ESC' : e.key));
  post('/key', {key:code}).catch(() => showToast('按键发送失败'));
});
async function poll() {
  try {
    const r = await fetch('/api/windows', {cache:'no-store'});
    if (!r.ok) throw new Error();
    const wins = await r.json();
    const container = document.getElementById('container');
    const current = new Set(wins);
    for (const [name, card] of known) {
      if (!current.has(name)) { card.querySelector('img').src = ''; card.remove(); known.delete(name); }
    }
    wins.forEach(name => {
      if (known.has(name)) return;
      const card = document.createElement('section');
      card.className = 'card';
      const title = document.createElement('div');
      title.className = 'title'; title.textContent = name;
      const img = document.createElement('img');
      img.alt = name; img.src = '/stream?window=' + encodeURIComponent(name);
      card.append(title, img); container.appendChild(card); known.set(name, card);
    });
    document.getElementById('placeholder').hidden = wins.length > 0;
    document.getElementById('dot').className = 'dot';
    document.getElementById('status').textContent = '已连接 | ' + wins.length + ' 个窗口';
    document.getElementById('hdr-info').textContent = wins.join(', ');
  } catch {
    document.getElementById('dot').className = 'dot off';
    document.getElementById('status').textContent = '连接断开 ...';
  }
}
async function pollControls(force = false) {
  try {
    const r = await fetch('/api/trackbars', {cache:'no-store'});
    if (!r.ok) throw new Error();
    const tbs = await r.json();
    document.getElementById('controls').hidden = !tbs.length;
    document.getElementById('layout').classList.toggle('has-controls', tbs.length > 0);
    const current = new Set();
    tbs.forEach(tb => {
      const key = JSON.stringify([tb.window, tb.name]);
      current.add(key);
      let el = controls.get(key);
      if (!el) {
        el = document.createElement('div'); el.className = 'trackbar';
        const label = document.createElement('label');
        const val = document.createElement('span'); val.className = 'val';
        const input = document.createElement('input');
        input.type = 'range'; input.min = 0; input.id = 'slider-' + (++pollControls.nextId);
        label.htmlFor = input.id;
        input.addEventListener('input', () => { val.textContent = input.value; });
        input.addEventListener('change', () => {
          const value = Number(input.value);
          input.dataset.pending = 'true';
          pendingUpdates = pendingUpdates.then(() => post('/api/trackbar/update', {
            name:tb.name, window:tb.window, value
          })).catch(e => {
            updateFailed = true;
            showToast('参数更新失败：' + e.message);
          }).finally(() => {
            delete input.dataset.pending;
          });
        });
        const note = document.createElement('span'); note.className = 'parameter-note';
        el.append(label, val, input, note); controls.set(key, el);
        const page = tb.window === 'Camera Settings' ? 'camera' : 'opencv';
        document.getElementById('trackbars-' + page).appendChild(el);
      }
      const input = el.querySelector('input');
      input.min = tb.min === undefined ? 0 : tb.min;
      input.max = tb.max;
      input.step = tb.step === undefined ? 1 : tb.step;
      input.dataset.enabled = String(tb.enabled !== false);
      input.disabled = parameterAction || tb.enabled === false;
      el.querySelector('label').textContent = tb.name + (tb.unit ? ' (' + tb.unit + ')' : '');
      const note = el.querySelector('.parameter-note');
      note.textContent = tb.reason || ''; note.hidden = !tb.reason;
      if (force || (document.activeElement !== input && !input.dataset.pending)) {
        input.value = tb.value;
        el.querySelector('.val').textContent = Number(Number(tb.value).toFixed(3));
      }
    });
    for (const [key, el] of controls) {
      if (!current.has(key)) { el.remove(); controls.delete(key); }
    }
    ['opencv', 'camera'].forEach(page => {
      document.getElementById('empty-' + page).hidden =
        document.getElementById('trackbars-' + page).childElementCount > 0;
    });
  } catch { if (force) throw new Error('无法读取参数'); }
}
pollControls.nextId = 0;
async function parameterRequest(action) {
  parameterAction = true;
  document.querySelectorAll('.controls button,.controls input').forEach(el => { el.disabled = true; });
  try {
    await pendingUpdates;
    if (updateFailed && action === 'save') {
      updateFailed = false;
      throw new Error('有参数更新失败，请确认滑块值后重试');
    }
    await post('/api/trackbars/' + action);
    updateFailed = false;
    await pollControls(true);
    showToast(action === 'save' ? '参数已写入 YAML 配置文件' : '已恢复之前参数');
  } catch (e) { showToast('操作失败：' + e.message); }
  finally {
    parameterAction = false;
    document.querySelectorAll('.controls button').forEach(el => { el.disabled = false; });
    document.querySelectorAll('.controls input').forEach(el => { el.disabled = el.dataset.enabled === 'false'; });
  }
}
document.getElementById('save-params').addEventListener('click', () => parameterRequest('save'));
document.getElementById('restore-params').addEventListener('click', () => parameterRequest('restore'));
setInterval(poll, 2000);
setInterval(() => pollControls(), 2000);
poll(); pollControls();
</script>
</body>
</html>)html";

// ---------------------------------------------------------------------------
// WebViewer implementation
// ---------------------------------------------------------------------------

WebViewer::WebViewer(int port, int jpeg_quality, int max_fps)
    : port_(port), jpeg_quality_(jpeg_quality) {
    setMaxFps(max_fps);
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        tools::logger()->error("[WebViewer] socket() 失败: {}", strerror(errno));
        //std::cerr << "[WebViewer] socket() 失败: " << strerror(errno)
        //          << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
        0) {
        tools::logger()->error("[WebViewer] bind() 端口 {} 失败: {}", port_, strerror(errno));
        //std::cerr << "[WebViewer] bind() 端口 " << port_ << " 失败: "
        //          << strerror(errno) << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (listen(server_fd_, 32) < 0) {
        tools::logger()->error("[WebViewer] listen() 失败: {}", strerror(errno));
        //std::cerr << "[WebViewer] listen() 失败: " << strerror(errno)
        //          << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    running_       = true;
    server_thread_ = std::thread(&WebViewer::server_loop, this);

    tools::logger()->info("[WebViewer] 服务已启动: http://localhost:{}", port_);
    //std::cout << "[WebViewer] 服务已启动: http://localhost:" << port_
    //          << std::endl;
}

WebViewer::~WebViewer() {
    running_ = false;
    win_cv_.notify_all();
    key_cv_.notify_all();

    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    if (server_thread_.joinable()) server_thread_.join();

    {
        std::lock_guard<std::mutex> lock(fds_mtx_);
        for (int fd : active_fds_) shutdown(fd, SHUT_RDWR);
    }

    while (active_handlers_ > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void WebViewer::namedWindow(const std::string& winname) {
    std::lock_guard<std::mutex> lock(win_mtx_);
    windows_[winname];
}

void WebViewer::setMaxFps(int max_fps) {
    if (max_fps <= 0) throw std::invalid_argument("WebViewer max_fps must be positive");
    max_fps_ = max_fps;
}

void WebViewer::imshow(const std::string& winname, const cv::Mat& img) {
    std::lock_guard<std::mutex> lock(win_mtx_);
    auto& wd = windows_[winname];
    img.copyTo(wd.frame);
    ++wd.seq;
    win_cv_.notify_all();
}

int WebViewer::waitKey(int delay_ms) {
    std::unique_lock<std::mutex> lock(key_mtx_);
    auto pred = [this] { return !keys_.empty() || !running_; };

    if (delay_ms <= 0)
        key_cv_.wait(lock, pred);
    else
        key_cv_.wait_for(lock, std::chrono::milliseconds(delay_ms), pred);

    if (!keys_.empty()) {
        int k = keys_.front();
        keys_.pop();
        return k;
    }
    return -1;
}

void WebViewer::destroyWindow(const std::string& winname) {
    std::lock_guard<std::mutex> lock(win_mtx_);
    windows_.erase(winname);
}

void WebViewer::destroyAllWindows() {
    std::lock_guard<std::mutex> lock(win_mtx_);
    windows_.clear();
}

// ---------------------------------------------------------------------------
// server
// ---------------------------------------------------------------------------

void WebViewer::server_loop() {
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t   addr_len  = sizeof(client_addr);
        int         client_fd = accept(
            server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (running_)
                tools::logger()->error("[WebViewer] accept() 失败: {}", strerror(errno));
                //std::cerr << "[WebViewer] accept() 失败: " << strerror(errno)
                //          << std::endl;
            continue;
        }

        // 5s 读写超时，防止线程永远阻塞
        struct timeval tv { 5, 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ++active_handlers_;
        std::thread([this, client_fd] {
            {
                std::lock_guard<std::mutex> lock(fds_mtx_);
                active_fds_.insert(client_fd);
            }
            handle_client(client_fd);

            {
                std::lock_guard<std::mutex> lock(fds_mtx_);
                active_fds_.erase(client_fd);
            }
            close(client_fd);
            --active_handlers_;
        }).detach();
    }
}

// ---------------------------------------------------------------------------
// request handling
// ---------------------------------------------------------------------------

void WebViewer::handle_client(int fd) {
    std::string request;
    size_t body_start = std::string::npos;
    size_t content_length = 0;
    while (true) {
        char buf[4096];
        const int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        request.append(buf, static_cast<size_t>(n));
        if (request.size() > 16384) {
            send_response(fd, 413, "text/plain", "Request too large");
            return;
        }
        if (body_start == std::string::npos) {
            const auto end = request.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            body_start = end + 4;
            std::istringstream headers(request.substr(0, end));
            std::string line;
            std::getline(headers, line);
            while (std::getline(headers, line)) {
                auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = line.substr(0, colon);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (name == "content-length") {
                    try {
                        auto length = line.substr(colon + 1);
                        size_t used = 0;
                        content_length = std::stoul(length, &used);
                        if (length.find_first_not_of(" \t\r", used) != std::string::npos ||
                            content_length > 8192) throw std::invalid_argument("length");
                    } catch (...) {
                        send_response(fd, 400, "text/plain", "Invalid Content-Length");
                        return;
                    }
                }
            }
        }
        if (request.size() >= body_start + content_length) break;
    }

    auto line_end = request.find("\r\n");
    if (line_end == std::string::npos) return;

    std::istringstream iss(request.substr(0, line_end));
    std::string        method, path, version;
    iss >> method >> path >> version;

    // 取 path 的基础部分（不含 query string）
    std::string base_path = path;
    auto        qpos      = path.find('?');
    if (qpos != std::string::npos) base_path = path.substr(0, qpos);

    if (method == "GET") {
        if (base_path == "/") {
            send_html_page(fd);
        } else if (base_path == "/stream") {
            send_mjpeg_stream(fd, query_param(path, "window"));
        } else if (base_path == "/api/windows") {
            send_window_list(fd);
        } else if (base_path == "/api/trackbars") {
            send_trackbar_list(fd);
        } else {
            send_response(fd, 404, "text/plain", "Not Found");
        }
    } else if (method == "POST" && base_path == "/key") {
        auto body_start = request.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            std::string body     = request.substr(body_start + 4);
            auto        key_pos  = body.find("\"key\"");
            if (key_pos != std::string::npos) {
                auto colon = body.find(':', key_pos);
                if (colon != std::string::npos) {
                    try {
                        int key_code = std::stoi(body.substr(colon + 1));
                        {
                            std::lock_guard<std::mutex> lock(key_mtx_);
                            keys_.push(key_code);
                        }
                        key_cv_.notify_one();
                    } catch (...) {}
                }
            }
        }
        send_response(fd, 200, "text/plain", "OK");
    } else if (method == "POST" && base_path == "/api/trackbars/save") {
        handle_parameter_action(fd, true);
    } else if (method == "POST" && base_path == "/api/trackbars/restore") {
        handle_parameter_action(fd, false);
    } else if (method == "POST" && base_path == "/api/trackbar/update") {
        try {
            const auto body = nlohmann::json::parse(request.substr(body_start, content_length));
            const auto name = body.at("name").get<std::string>();
            const auto window = body.value("window", std::string{});
            if (!body.at("value").is_number()) throw std::invalid_argument("value");
            const double value = body.at("value").get<double>();
            if (!std::isfinite(value)) throw std::invalid_argument("value");
            std::lock_guard<std::mutex> lock(tb_mtx_);
            auto target = trackbars_.end();
            for (auto it = trackbars_.begin(); it != trackbars_.end(); ++it) {
                if (it->name != name || (!window.empty() && it->winname != window)) continue;
                if (target != trackbars_.end()) throw std::invalid_argument("ambiguous trackbar");
                target = it;
            }
            if (target == trackbars_.end()) {
                send_response(fd, 404, "text/plain", "Trackbar not found");
                return;
            }
            if (target->read) {
                const auto state = target->read();
                if (!state.enabled) throw std::runtime_error(state.reason);
                if (value < state.minimum || value > state.maximum)
                    throw std::invalid_argument("参数超出相机支持范围");
                target->write(value);
            } else {
                if (std::floor(value) != value || value < target->min_val || value > target->max_val)
                    throw std::invalid_argument("参数超出滑块范围或不是整数");
                if (target->value_ptr) *target->value_ptr = static_cast<int>(value);
                if (target->onChange) target->onChange(static_cast<int>(value), target->userdata);
            }
            send_response(fd, 200, "text/plain", "OK");
        } catch (const std::exception& e) {
            send_response(fd, 400, "text/plain; charset=utf-8", e.what());
        }
    } else if (method == "OPTIONS") {
        // CORS preflight
        std::string headers =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n\r\n";
        send(fd, headers.data(), headers.size(), MSG_NOSIGNAL);
    } else {
        send_response(fd, 405, "text/plain", "Method Not Allowed");
    }
}

// ---------------------------------------------------------------------------
// response helpers
// ---------------------------------------------------------------------------

void WebViewer::send_response(int fd, int code,
                              const std::string& content_type,
                              const std::string& body) {
    const char* status_text = "OK";
    switch (code) {
        case 200: status_text = "200 OK"; break;
        case 204: status_text = "204 No Content"; break;
        case 400: status_text = "400 Bad Request"; break;
        case 409: status_text = "409 Conflict"; break;
        case 413: status_text = "413 Payload Too Large"; break;
        case 500: status_text = "500 Internal Server Error"; break;
        case 404: status_text = "404 Not Found"; break;
        case 405: status_text = "405 Method Not Allowed"; break;
        default:  status_text = "200 OK"; break;
    }
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_text << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    std::string resp = oss.str();
    size_t sent = 0;
    while (sent < resp.size()) {
        const auto n = send(fd, resp.data() + sent, resp.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

void WebViewer::send_html_page(int fd) {
    send_response(fd, 200, "text/html; charset=utf-8", HTML_PAGE);
}

void WebViewer::send_window_list(int fd) {
    std::lock_guard<std::mutex> lock(win_mtx_);
    auto json = nlohmann::json::array();
    for (const auto& [name, data] : windows_) {
        // 纯滑块窗口没有图像，避免产生空白视频卡片和无效长连接。
        if (!data.frame.empty()) json.push_back(name);
    }
    send_response(fd, 200, "application/json", json.dump());
}

void WebViewer::send_trackbar_list(int fd) {
    std::lock_guard<std::mutex> lock(tb_mtx_);
    auto json = nlohmann::json::array();
    for (const auto& tb : trackbars_) {
        if (tb.read) {
            const auto state = tb.read();
            json.push_back({{"name", tb.name}, {"window", tb.winname}, {"value", state.value},
                            {"min", state.minimum}, {"max", state.maximum}, {"step", state.step},
                            {"enabled", state.enabled}, {"reason", state.reason}, {"unit", state.unit}});
        } else json.push_back({{"name", tb.name}, {"window", tb.winname},
                        {"value", tb.value_ptr ? *tb.value_ptr : 0},
                        {"min", tb.min_val}, {"max", tb.max_val}});
    }
    send_response(fd, 200, "application/json", json.dump());
}

void WebViewer::createDynamicTrackbar(const std::string& name, const std::string& window,
                                      std::function<SliderState()> read,
                                      std::function<void(double)> write) {
    std::lock_guard<std::mutex> lock(tb_mtx_);
    TrackbarData data{name, window, nullptr, 0, nullptr, nullptr, 0, std::move(read), std::move(write)};
    for (auto& tb : trackbars_) {
        if (tb.name == name && tb.winname == window) { tb = std::move(data); return; }
    }
    trackbars_.push_back(std::move(data));
}

void WebViewer::setParameterActions(std::function<void()> save, std::function<void()> restore) {
    std::lock_guard<std::mutex> lock(tb_mtx_);
    save_parameters_ = std::move(save);
    restore_parameters_ = std::move(restore);
}

void WebViewer::addParameterActions(std::function<void()> save, std::function<void()> restore) {
    std::lock_guard<std::mutex> lock(tb_mtx_);
    auto previous_save = save_parameters_;
    auto previous_restore = restore_parameters_;
    save_parameters_ = [previous_save, save] { if (previous_save) previous_save(); save(); };
    restore_parameters_ = [previous_restore, restore] { restore(); if (previous_restore) previous_restore(); };
}

void WebViewer::handle_parameter_action(int fd, bool save) {
    std::lock_guard<std::mutex> lock(tb_mtx_);
    const auto& action = save ? save_parameters_ : restore_parameters_;
    if (trackbars_.empty() || !action) {
        send_response(fd, 409, "text/plain; charset=utf-8", "未开启参数保存/恢复功能");
        return;
    }
    try {
        action();
        send_response(fd, 200, "text/plain", "OK");
    } catch (const std::exception& e) {
        tools::logger()->error("[WebViewer] 参数{}失败: {}", save ? "保存" : "恢复", e.what());
        send_response(fd, 500, "text/plain; charset=utf-8",
                      std::string(save ? "无法保存参数：" : "无法恢复参数：") + e.what());
    }
}

// ---------------------------------------------------------------------------
// MJPEG streaming
// ---------------------------------------------------------------------------

void WebViewer::send_mjpeg_stream(int fd, const std::string& window) {
    // MJPEG 流不设置读写超时，保持长连接
    struct timeval tv_long { 0, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_long, sizeof(tv_long));

    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send(fd, header.data(), header.size(), MSG_NOSIGNAL) < 0) return;

    uint64_t          last_seq = 0;
    std::vector<uchar> jpeg_buf;
    std::vector<int>   params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality_};
    auto next_frame_time = std::chrono::steady_clock::now();

    while (running_) {
        cv::Mat frame;
        {
            std::unique_lock<std::mutex> lock(win_mtx_);
            win_cv_.wait_until(lock, next_frame_time, [this] { return !running_; });
            if (!running_) break;
            win_cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                auto it = windows_.find(window);
                return (it != windows_.end() && it->second.seq > last_seq) ||
                       !running_;
            });
            if (!running_) break;

            auto it = windows_.find(window);
            if (it == windows_.end() || it->second.seq == last_seq ||
                it->second.frame.empty())
                continue;

            frame    = it->second.frame.clone();
            last_seq = it->second.seq;
            next_frame_time = std::chrono::steady_clock::now() +
                std::chrono::nanoseconds(1000000000LL / max_fps_.load());
        }

        jpeg_buf.clear();
        if (!cv::imencode(".jpg", frame, jpeg_buf, params)) continue;

        std::ostringstream part;
        part << "--frame\r\n"
             << "Content-Type: image/jpeg\r\n"
             << "Content-Length: " << jpeg_buf.size() << "\r\n"
             << "\r\n";
        std::string hdr = part.str();

        if (send(fd, hdr.data(), hdr.size(), MSG_NOSIGNAL) < 0) break;
        if (send(fd, reinterpret_cast<const char*>(jpeg_buf.data()),
                 jpeg_buf.size(), MSG_NOSIGNAL) < 0)
            break;
        if (send(fd, "\r\n", 2, MSG_NOSIGNAL) < 0) break;
    }
}

void WebViewer::createTrackbar(const std::string& trackbarname, const std::string& winname,
                               int* value, int count, TrackbarCallback onChange, void* userdata,
                               int min_value) {
    std::lock_guard<std::mutex> lock(tb_mtx_);
    // 更新或新增
    for (auto& tb : trackbars_) {
        if (tb.name == trackbarname && tb.winname == winname) {
            tb.value_ptr = value;
            tb.max_val = count;
            tb.min_val = min_value;
            tb.onChange = onChange;
            tb.userdata = userdata;
            return;
        }
    }
    trackbars_.push_back({trackbarname, winname, value, count, onChange, userdata, min_value});
}

}  // namespace tools