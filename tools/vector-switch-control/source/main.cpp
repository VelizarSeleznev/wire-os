#include <switch.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <vector>
#include <array>
#include <string>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <sys/stat.h>

constexpr int kScreenWidth = 1280;
constexpr int kScreenHeight = 720;
constexpr int kCamWidth = 640;
constexpr int kCamHeight = 360;
constexpr int kRobotPort = 8080;

// Structures for linear graphics
struct Canvas {
    uint32_t* pixels = nullptr;
    uint32_t stride = 0; // line stride in bytes
};

using Glyph = std::array<uint8_t, 7>;

// Simple embedded font glyph data (uppercase letters, numbers, and basic symbols)
Glyph glyph_for(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
        case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
        case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
        case 'D': return {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C};
        case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
        case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
        case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
        case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case 'J': return {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
        case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
        case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
        case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
        case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
        case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
        case '1': return {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F};
        case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        case '3': return {0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x1E};
        case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
        case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
        case ':': return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
        case '/': return {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
        case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        case '_': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
        case '+': return {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
        case ' ': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        default: return {0x1F, 0x11, 0x05, 0x02, 0x00, 0x02, 0x00};
    }
}

// Basic software drawing routines
void fill_rect(Canvas& canvas, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(kScreenWidth, x + w);
    const int y1 = std::min(kScreenHeight, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        auto* row = reinterpret_cast<uint8_t*>(canvas.pixels) + static_cast<size_t>(yy) * canvas.stride;
        auto* out = reinterpret_cast<uint32_t*>(row);
        for (int xx = x0; xx < x1; ++xx) {
            out[xx] = color;
        }
    }
}

void stroke_rect(Canvas& canvas, int x, int y, int w, int h, uint32_t color) {
    fill_rect(canvas, x, y, w, 2, color);
    fill_rect(canvas, x, y + h - 2, w, 2, color);
    fill_rect(canvas, x, y, 2, h, color);
    fill_rect(canvas, x + w - 2, y, 2, h, color);
}

void draw_glyph(Canvas& canvas, int x, int y, char c, int scale, uint32_t color) {
    const auto glyph = glyph_for(c);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((glyph[row] & (1u << (4 - col))) == 0) continue;
            fill_rect(canvas, x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

void draw_text(Canvas& canvas, int x, int y, const std::string& text, int scale, uint32_t color) {
    int cursor_x = x;
    int cursor_y = y;
    for (char c : text) {
        if (c == '\n') {
            cursor_x = x;
            cursor_y += scale * 9;
            continue;
        }
        draw_glyph(canvas, cursor_x, cursor_y, c, scale, color);
        cursor_x += scale * 6;
    }
}

// Config loading and saving helpers
const std::string kConfigDir = "sdmc:/switch/vector-switch-control";
const std::string kConfigFile = kConfigDir + "/config.txt";

bool load_config_ip(std::string& ip) {
    FILE* f = std::fopen(kConfigFile.c_str(), "r");
    if (!f) return false;
    char buf[128];
    if (std::fgets(buf, sizeof(buf), f)) {
        ip = buf;
        // Trim trailing newline or whitespace
        ip.erase(std::remove_if(ip.begin(), ip.end(), [](unsigned char x) {
            return std::isspace(x);
        }), ip.end());
        std::fclose(f);
        return !ip.empty();
    }
    std::fclose(f);
    return false;
}

void save_config_ip(const std::string& ip) {
    mkdir("sdmc:/switch", 0755);
    mkdir(kConfigDir.c_str(), 0755);
    FILE* f = std::fopen(kConfigFile.c_str(), "w");
    if (f) {
        std::fprintf(f, "%s\n", ip.c_str());
        std::fclose(f);
    }
}

// Interactive Software Keyboard IP input using system OS applet
bool get_ip_from_keyboard(std::string& ip) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) return false;

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "Vector IP Configuration");
    swkbdConfigSetGuideText(&kbd, "Enter Vector's IP address (e.g., 192.168.1.89)");
    if (!ip.empty()) {
        swkbdConfigSetInitialText(&kbd, ip.c_str());
    } else {
        swkbdConfigSetInitialText(&kbd, "192.168.1.89");
    }

    char out_str[128] = {0};
    rc = swkbdShow(&kbd, out_str, sizeof(out_str));
    swkbdClose(&kbd);

    if (R_SUCCEEDED(rc)) {
        ip = out_str;
        // Remove whitespace
        ip.erase(std::remove_if(ip.begin(), ip.end(), [](unsigned char x) {
            return std::isspace(x);
        }), ip.end());
        return !ip.empty();
    }
    return false;
}

// HTTP POST client for motor commands (non-blocking, short timeout)
bool http_post_motors(const std::string& ip, int port, float left, float right, float lift, float head) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval timeout{0, 60000}; // 60ms timeout to avoid UI blocking
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(sock);
        return false;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    char json_body[256];
    std::snprintf(json_body, sizeof(json_body),
                 "{\"left\":%.3f,\"right\":%.3f,\"lift\":%.3f,\"head\":%.3f,\"ttl_ms\":350}",
                 left, right, lift, head);

    std::string request = "POST /v1/motors HTTP/1.1\r\n"
                          "Host: " + ip + ":" + std::to_string(port) + "\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " + std::to_string(std::strlen(json_body)) + "\r\n"
                          "Connection: close\r\n\r\n" + json_body;

    send(sock, request.data(), request.size(), 0);
    close(sock);
    return true;
}

// Background Multi-threaded Camera State
std::mutex g_frame_mutex;
std::vector<uint8_t> g_latest_frame;
std::atomic<bool> g_thread_running{true};
std::atomic<bool> g_camera_connected{false};
std::atomic<int> g_fps_counter{0};

void camera_fetching_thread(std::string ip) {
    const std::string separator = "\r\n\r\n";
    const std::vector<uint8_t> bm_sig = { 0x42, 0x4d }; // 'B', 'M'
    std::vector<uint8_t> stream_buf;

    while (g_thread_running.load()) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // 3-second timeout for streaming read
        struct timeval timeout{3, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kRobotPort);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            close(sock);
            g_camera_connected.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            g_camera_connected.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Establish the persistent stream
        std::string request = "GET /v1/camera/stream HTTP/1.1\r\n"
                              "Host: " + ip + ":" + std::to_string(kRobotPort) + "\r\n"
                              "Accept: multipart/x-mixed-replace\r\n"
                              "Connection: keep-alive\r\n\r\n";

        if (send(sock, request.data(), request.size(), 0) < 0) {
            close(sock);
            g_camera_connected.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        stream_buf.clear();
        char recv_buf[32768]; // 32KB read buffer
        bool http_header_passed = false;
        int local_fps = 0;
        auto last_fps_time = std::chrono::steady_clock::now();

        while (g_thread_running.load()) {
            ssize_t n = recv(sock, recv_buf, sizeof(recv_buf), 0);
            if (n <= 0) {
                break; // Connection lost, let's reconnect
            }

            stream_buf.insert(stream_buf.end(), recv_buf, recv_buf + n);

            // Skip HTTP response header first
            if (!http_header_passed) {
                auto it = std::search(stream_buf.begin(), stream_buf.end(), separator.begin(), separator.end());
                if (it != stream_buf.end()) {
                    stream_buf.erase(stream_buf.begin(), it + separator.size());
                    http_header_passed = true;
                } else {
                    if (stream_buf.size() > 8192) break; // Safety cutoff
                    continue;
                }
            }

            // Extract frames from the continuous buffer
            while (stream_buf.size() >= 54) {
                auto bm_it = std::search(stream_buf.begin(), stream_buf.end(), bm_sig.begin(), bm_sig.end());
                if (bm_it == stream_buf.end()) {
                    if (stream_buf.size() > 1) {
                        stream_buf.erase(stream_buf.begin(), stream_buf.end() - 1);
                    }
                    break;
                }

                if (bm_it != stream_buf.begin()) {
                    stream_buf.erase(stream_buf.begin(), bm_it);
                    continue;
                }

                // Fixed BMP frame size in 640x360 24-bit uncompressed
                constexpr size_t kBmpSize = 691254;
                if (stream_buf.size() < kBmpSize) {
                    break; // Frame is incomplete, wait for more data
                }

                std::vector<uint8_t> frame(stream_buf.begin(), stream_buf.begin() + kBmpSize);
                stream_buf.erase(stream_buf.begin(), stream_buf.begin() + kBmpSize);

                {
                    std::lock_guard<std::mutex> lock(g_frame_mutex);
                    g_latest_frame = std::move(frame);
                }
                g_camera_connected.store(true);
                local_fps++;

                auto now = std::chrono::steady_clock::now();
                if (now - last_fps_time >= std::chrono::seconds(1)) {
                    g_fps_counter.store(local_fps);
                    local_fps = 0;
                    last_fps_time = now;
                }
            }
        }

        close(sock);
        g_camera_connected.store(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

// 2x Scaled BMP Renderer on Canvas
void draw_bmp_frame(Canvas& canvas, const std::vector<uint8_t>& bmp_data) {
    if (bmp_data.size() < 54) return;

    const uint8_t* bmp_start = bmp_data.data();
    if (bmp_start[0] != 'B' || bmp_start[1] != 'M') return;

    uint32_t pixel_offset = *reinterpret_cast<const uint32_t*>(bmp_start + 10);
    const uint8_t* pixel_data = bmp_start + pixel_offset;

    // Direct 2x scaling: BGR (BMP) -> RGBA (Switch linear framebuffer)
    // Flipped to normal top-down mapping (bmp_y = y) to correct the camera orientation
    for (int y = 0; y < kCamHeight; ++y) {
        int bmp_y = y; 
        const uint8_t* bmp_row = pixel_data + bmp_y * (kCamWidth * 3);

        auto* target_row0 = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(canvas.pixels) + (y * 2) * canvas.stride);
        auto* target_row1 = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(canvas.pixels) + (y * 2 + 1) * canvas.stride);

        for (int x = 0; x < kCamWidth; ++x) {
            uint8_t b = bmp_row[x * 3];
            uint8_t g = bmp_row[x * 3 + 1];
            uint8_t r = bmp_row[x * 3 + 2];

            uint32_t color = RGBA8_MAXALPHA(r, g, b);

            target_row0[x * 2]     = color;
            target_row0[x * 2 + 1] = color;
            target_row1[x * 2]     = color;
            target_row1[x * 2 + 1] = color;
        }
    }
}

// Renders the glowing green technical grid background when offline
void draw_cyber_grid(Canvas& canvas, uint64_t frame_tick) {
    // Solid background: very dark green/gray
    fill_rect(canvas, 0, 0, kScreenWidth, kScreenHeight, RGBA8_MAXALPHA(10, 16, 12));

    // Dynamic grid spacing
    int offset = static_cast<int>((frame_tick / 2) % 40);
    
    // Vertical grid lines
    for (int x = offset; x < kScreenWidth; x += 40) {
        fill_rect(canvas, x, 0, 1, kScreenHeight, RGBA8_MAXALPHA(20, 50, 32));
    }
    // Horizontal grid lines
    for (int y = offset; y < kScreenHeight; y += 40) {
        fill_rect(canvas, 0, y, kScreenWidth, 1, RGBA8_MAXALPHA(20, 50, 32));
    }

    // Outer border decoration
    stroke_rect(canvas, 20, 20, kScreenWidth - 40, kScreenHeight - 40, RGBA8_MAXALPHA(0, 255, 100));
}

// HUD overlays on top of the live video
void draw_hud_overlays(Canvas& canvas, const std::string& ip, float left, float right, float lift, float head) {
    // Crosshair in the exact center of the screen
    constexpr int cx = kScreenWidth / 2;
    constexpr int cy = kScreenHeight / 2;
    fill_rect(canvas, cx - 15, cy, 30, 2, RGBA8_MAXALPHA(0, 255, 128));
    fill_rect(canvas, cx, cy - 15, 2, 30, RGBA8_MAXALPHA(0, 255, 128));
    stroke_rect(canvas, cx - 4, cy - 4, 8, 8, RGBA8_MAXALPHA(0, 255, 128));

    // Translucent HUD dashboard bars at the top & bottom
    fill_rect(canvas, 0, 0, kScreenWidth, 46, RGBA8_MAXALPHA(15, 20, 18));
    fill_rect(canvas, 0, kScreenHeight - 46, kScreenWidth, 46, RGBA8_MAXALPHA(15, 20, 18));
    fill_rect(canvas, 0, 46, kScreenWidth, 2, RGBA8_MAXALPHA(0, 255, 128));
    fill_rect(canvas, 0, kScreenHeight - 48, kScreenWidth, 2, RGBA8_MAXALPHA(0, 255, 128));

    // Text details (Top HUD)
    draw_text(canvas, 24, 15, "VECTOR ROBOT TACTICAL CONSOLE", 2, RGBA8_MAXALPHA(220, 255, 230));
    draw_text(canvas, 600, 15, "STREAM IP:" + ip, 2, RGBA8_MAXALPHA(0, 255, 128));
    
    int fps = g_fps_counter.load();
    draw_text(canvas, 1140, 15, "FPS:" + std::to_string(fps), 2, fps > 0 ? RGBA8_MAXALPHA(0, 255, 128) : RGBA8_MAXALPHA(255, 100, 100));

    // Motor Diagnostic telemetry (Bottom HUD)
    char motor_diag[256];
    std::snprintf(motor_diag, sizeof(motor_diag), 
                  "TREADS L:%+.2f R:%+.2f | TILT HEAD:%+.2f | LIFT:%+.2f", 
                  left, right, head, lift);
    draw_text(canvas, 24, kScreenHeight - 31, motor_diag, 2, RGBA8_MAXALPHA(200, 255, 210));
    draw_text(canvas, 1080, kScreenHeight - 31, "PRESS + TO QUIT", 2, RGBA8_MAXALPHA(255, 140, 140));
}

int main(int argc, char** argv) {
    romfsInit();
    socketInitializeDefault();
    fsdevMountSdmc();

    // Try loading IP from SD config, otherwise use default robot IP
    std::string ip_address = "";
    bool config_loaded = load_config_ip(ip_address);
    if (!config_loaded || ip_address.empty()) {
        ip_address = "192.168.1.89";
        save_config_ip(ip_address);
    }
    
    // Create Linear Framebuffer for double-buffered rendering
    Framebuffer framebuffer{};
    if (R_FAILED(framebufferCreate(&framebuffer, nwindowGetDefault(), kScreenWidth, kScreenHeight, PIXEL_FORMAT_RGBA_8888, 2))) {
        consoleInit(NULL);
        printf("Failed to create linear framebuffer.\n");
        consoleUpdate(NULL);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }
    framebufferMakeLinear(&framebuffer);

    // Configure Joy-Con inputs
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    // Spin up background camera frame fetcher
    g_thread_running.store(true);
    std::thread cam_thread(camera_fetching_thread, ip_address);

    uint64_t frame_tick = 0;
    float current_l = 0.0f, current_r = 0.0f, current_lift = 0.0f, current_head = 0.0f;

    while (appletMainLoop()) {
        padUpdate(&pad);
        uint64_t buttons_down = padGetButtons(&pad);

        // Exit immediately when + is pressed
        if (buttons_down & HidNpadButton_Plus) {
            break;
        }

        // Joystick analog values
        HidAnalogStickState stick_l = padGetStickPos(&pad, 0);
        HidAnalogStickState stick_r = padGetStickPos(&pad, 1);

        // Normalize axes (-1.0 to 1.0)
        float target_l = stick_l.y / 32767.0f;
        float target_r = stick_r.y / 32767.0f;

        // Apply deadbands
        if (std::abs(target_l) < 0.15f) target_l = 0.0f;
        if (std::abs(target_r) < 0.15f) target_r = 0.0f;

        // Head controls: R/L for fast (0.8), D-pad Right/Left for slow (0.3)
        float target_head = 0.0f;
        if (buttons_down & HidNpadButton_R) target_head = 0.8f;               // Head Up Fast
        else if (buttons_down & HidNpadButton_L) target_head = -0.8f;         // Head Down Fast
        else if (buttons_down & HidNpadButton_Right) target_head = 0.3f;      // Head Up Slow
        else if (buttons_down & HidNpadButton_Left) target_head = -0.3f;       // Head Down Slow

        // Lift controls: ZR/ZL for fast (0.8), D-pad Up/Down for slow (0.3)
        float target_lift = 0.0f;
        if (buttons_down & HidNpadButton_ZR) target_lift = 0.8f;              // Lift Up Fast
        else if (buttons_down & HidNpadButton_ZL) target_lift = -0.8f;        // Lift Down Fast
        else if (buttons_down & HidNpadButton_Up) target_lift = 0.3f;         // Lift Up Slow
        else if (buttons_down & HidNpadButton_Down) target_lift = -0.3f;       // Lift Down Slow


        // Only issue network motor command POSTs if there is actual motion command or changes
        // This dramatically reduces network clogging
        bool moving = (target_l != 0.0f || target_r != 0.0f || target_lift != 0.0f || target_head != 0.0f);
        bool was_moving = (current_l != 0.0f || current_r != 0.0f || current_lift != 0.0f || current_head != 0.0f);

        if (moving || was_moving) {
            http_post_motors(ip_address, kRobotPort, target_l, target_r, target_lift, target_head);
            current_l = target_l;
            current_r = target_r;
            current_lift = target_lift;
            current_head = target_head;
        }

        // Get linear frame buffer
        uint32_t stride = 0;
        uint32_t* pixels = static_cast<uint32_t*>(framebufferBegin(&framebuffer, &stride));
        Canvas canvas{pixels, stride};

        // Render scene
        bool camera_active = g_camera_connected.load();
        if (camera_active) {
            std::vector<uint8_t> local_frame;
            {
                std::lock_guard<std::mutex> lock(g_frame_mutex);
                local_frame = g_latest_frame;
            }
            draw_bmp_frame(canvas, local_frame);
        } else {
            // Draw technical cyber grid while warming up or disconnected
            draw_cyber_grid(canvas, frame_tick);
            
            // Connecting overlay diagnostics
            draw_text(canvas, 100, 200, "CONNECTING TO TACTICAL ROBOT NODE...", 3, RGBA8_MAXALPHA(0, 255, 128));
            draw_text(canvas, 100, 260, "TARGET IP ADDRESS: " + ip_address + ":" + std::to_string(kRobotPort), 2, RGBA8_MAXALPHA(200, 255, 220));
            draw_text(canvas, 100, 310, "DIAGNOSTICS: CHECK VECTOR POWER & WI-FI SUBNET REACHABILITY", 2, RGBA8_MAXALPHA(140, 200, 160));
            
            // Allow manual IP re-entry by pressing Y
            draw_text(canvas, 100, 480, "PRESS Y TO CHANGE TARGET IP ADDRESS", 2, RGBA8_MAXALPHA(255, 230, 100));

            if (buttons_down & HidNpadButton_Y) {
                // Pause camera thread and launch keyboard applet
                g_thread_running.store(false);
                if (cam_thread.joinable()) cam_thread.join();
                
                if (get_ip_from_keyboard(ip_address)) {
                    save_config_ip(ip_address);
                }
                
                g_thread_running.store(true);
                cam_thread = std::thread(camera_fetching_thread, ip_address);
            }
        }

        // Superimpose the telemetry HUD
        draw_hud_overlays(canvas, ip_address, target_l, target_r, target_lift, target_head);

        // Draw current frames count and commit buffer
        framebufferEnd(&framebuffer);

        frame_tick++;
        // Maintain smooth 60Hz loop (framebufferEnd handles vertical sync natively, 
        // but we sleep slightly to ease the CPU thread scheduler).
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // Stop background thread safely
    g_thread_running.store(false);
    if (cam_thread.joinable()) {
        cam_thread.join();
    }

    // Ensure we zero out all robot motors upon exit
    http_post_motors(ip_address, kRobotPort, 0.0f, 0.0f, 0.0f, 0.0f);

    // Free standard Switch resources
    framebufferClose(&framebuffer);
    fsdevUnmountAll();
    socketExit();
    romfsExit();
    return 0;
}
