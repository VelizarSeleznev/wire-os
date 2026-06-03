#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <linux/spi/spidev.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kApiVersion = "0.2.7";
constexpr const char* kSpineDevice = "/dev/ttyHS0";
constexpr const char* kLcdDevice = "/dev/spidev1.0";
constexpr const char* kImuDevice = "/dev/spidev0.0";
constexpr int kDefaultPort = 8080;
constexpr int kDefaultTtlMs = 250;
constexpr int kMaxTtlMs = 5000;
constexpr int kLcdWidth = 184;
constexpr int kLcdHeight = 96;
constexpr int kLcdBytes = kLcdWidth * kLcdHeight * 2;
constexpr int kLcdMidasWidth = 160;
constexpr int kLcdMidasHeight = 80;
constexpr int kLcdMaxTransfer = 0x1000;
constexpr int kLcdDataClock = 17500000;
constexpr int kGpioLcdWrx = 110;
constexpr int kGpioLcdReset1 = 96;
constexpr int kGpioLcdReset2 = 55;
constexpr const char* kAudioUploadPath = "/tmp/vector-hw-audio.wav";
constexpr const char* kCameraSnapshotPath = "/tmp/vector-camera-snapshot.bmp";
constexpr const char* kAnkiCameraSocket = "/var/run/mm-anki-camera/camera-server";
constexpr int kAudioMixerMax = 74;
constexpr int kCameraStreamFps = 7;   // target fps for MJPEG stream
constexpr int kCameraStreamMs = 1000 / kCameraStreamFps;
constexpr int kMotorPositionTtlMs = 10000;  // max time for a position command
constexpr uint32_t kAnkiCameraMaxFrames = 6;
constexpr uint32_t kAnkiCameraMsgPayloadLen = 128;
constexpr uint32_t kAnkiCameraMsgClientHeartbeat = 0;
constexpr uint32_t kAnkiCameraMsgClientRegister = 1;
constexpr uint32_t kAnkiCameraMsgClientUnregister = 2;
constexpr uint32_t kAnkiCameraMsgClientStart = 3;
constexpr uint32_t kAnkiCameraMsgClientParams = 5;
constexpr uint32_t kAnkiCameraMsgServerStatus = 6;
constexpr uint32_t kAnkiCameraMsgServerBuffer = 7;
constexpr uint32_t kAnkiCameraParamsFormat = 2;
constexpr uint32_t kAnkiCameraFormatRgb888 = 1;

#pragma pack(push, 1)
struct MotorState {
  int32_t position;
  int32_t delta;
  uint32_t time;
};

struct BatteryState {
  int16_t main_voltage;
  int16_t charger;
  int16_t temperature;
  uint16_t flags;
  int16_t unused[2];
};

struct RangeData {
  uint8_t rangeStatus;
  uint8_t spare1;
  uint16_t rangeMM;
  uint16_t signalRate;
  uint16_t ambientRate;
  uint16_t spadCount;
  uint16_t sampleCount;
  uint32_t calibrationResult;
};

struct SpineHeader {
  uint32_t sync;
  uint16_t payloadType;
  uint16_t bytesToFollow;
};

struct BodyToHead {
  uint32_t framecounter;
  uint8_t flags;
  uint8_t tempAlarm;
  uint16_t failureCode;
  MotorState motor[4];
  uint16_t cliffSense[4];
  BatteryState battery;
  RangeData proximity;
  uint16_t touchLevel[2];
  uint16_t micError[2];
  uint16_t touchHires[2];
  uint8_t unused[24];
  int16_t audio[320];
};

struct MicroBodyToHead {
  uint8_t buttonPressed;
  uint8_t unused[3];
};

struct LightState {
  uint8_t ledColors[16];
};

struct HeadToBody {
  uint32_t framecounter;
  uint32_t powerFlags;
  int16_t motorPower[4];
  LightState lightState;
  uint8_t unused[32];
};

struct AnkiCameraMsg {
  uint32_t msg_id;
  uint32_t version;
  uint32_t client_id;
  int32_t fd;
  uint8_t payload[kAnkiCameraMsgPayloadLen];
};

struct AnkiCameraFrame {
  uint64_t timestamp;
  uint32_t frame_id;
  uint32_t width;
  uint32_t height;
  uint32_t bytes_per_row;
  uint8_t bits_per_pixel;
  uint8_t format;
  uint8_t reserved[2];
  uint32_t pad_to_64[8];
  uint8_t data[0];
};
#pragma pack(pop)

constexpr uint32_t kSyncBodyToHead = 0x483242aa;
constexpr uint32_t kSyncHeadToBody = 0x423248aa;
constexpr uint16_t kPayloadDataFrame = 0x6466;
constexpr uint16_t kPayloadBootFrame = 0x6662;

std::atomic<bool> gRunning{true};

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return crc;
}

bool exists(const std::string& path) {
  return access(path.c_str(), F_OK) == 0;
}

void joinSupplementaryGroupIfPresent(const char* groupName) {
  if (geteuid() != 0 || !groupName) return;
  group* gr = getgrnam(groupName);
  if (!gr) return;

  int groupCount = getgroups(0, nullptr);
  if (groupCount < 0) return;

  std::vector<gid_t> groups(static_cast<size_t>(groupCount));
  if (groupCount > 0 && getgroups(groupCount, groups.data()) < 0) return;

  if (std::find(groups.begin(), groups.end(), gr->gr_gid) != groups.end()) return;
  groups.push_back(gr->gr_gid);
  if (setgroups(static_cast<int>(groups.size()), groups.data()) != 0) {
    printf("[Init] failed to join group %s: %s\n", groupName, strerror(errno));
    fflush(stdout);
  }
}

std::string readFile(const std::string& path, size_t maxBytes = 1024 * 1024) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::ostringstream out;
  std::vector<char> buf(4096);
  size_t total = 0;
  while (in && total < maxBytes) {
    in.read(buf.data(), std::min(buf.size(), maxBytes - total));
    const auto got = static_cast<size_t>(in.gcount());
    out.write(buf.data(), got);
    total += got;
  }
  return out.str();
}

std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

std::string jsonEscape(const std::string& s) {
  std::ostringstream out;
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

bool writeTextFile(const std::string& path, const std::string& data) {
  int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n <= 0) {
      close(fd);
      return false;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  close(fd);
  return true;
}

bool writeAllFd(int fd, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  size_t left = len;
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n <= 0) return false;
    p += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

bool writeWholeFile(const std::string& path, const std::string& data, mode_t mode = 0600) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return false;
  bool ok = writeAllFd(fd, data.data(), data.size());
  close(fd);
  return ok;
}

int runCommand(const std::string& cmd) {
  int rc = system(cmd.c_str());
  if (rc < 0) return -1;
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  return 128;
}

bool processMatchesCommNonZombie(pid_t pid, const char* commName) {
  if (pid <= 0) return false;
  std::string procBase = std::string("/proc/") + std::to_string(pid);
  std::string comm = readFile(procBase + "/comm", 128);
  while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
  if (comm != commName) return false;

  std::string stat = readFile(procBase + "/stat", 512);
  size_t closeParen = stat.rfind(')');
  if (closeParen != std::string::npos && closeParen + 2 < stat.size()) {
    char state = stat[closeParen + 2];
    if (state == 'Z') return false;
  }
  return true;
}

pid_t findProcessByComm(const char* commName) {
  DIR* dir = opendir("/proc");
  if (!dir) return -1;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (!std::all_of(ent->d_name, ent->d_name + std::strlen(ent->d_name), ::isdigit)) continue;
    pid_t pid = static_cast<pid_t>(std::atoi(ent->d_name));
    if (processMatchesCommNonZombie(pid, commName)) {
      closedir(dir);
      return pid;
    }
  }
  closedir(dir);
  return -1;
}

std::atomic<int> gAudioVolumePercent{100};

// ── Camera daemon management ────────────────────────────────────────────────
std::atomic<pid_t> gCameraDaemonPid{-1};
std::atomic<pid_t> gAnkiCameraPid{-1};

void reapExitedChildren() {
  int status = 0;
  while (waitpid(-1, &status, WNOHANG) > 0) {}
}

void appendLe16(std::string& out, uint16_t v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void appendLe32(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 24) & 0xff));
}

bool startCameraDaemon(std::string& error) {
  reapExitedChildren();
  // Reap any zombie camera child from a previous run before checking PIDs.
  pid_t ankiPid = gAnkiCameraPid.load();
  if (ankiPid > 0) {
    int zombieStatus = 0;
    pid_t reaped = waitpid(ankiPid, &zombieStatus, WNOHANG);
    if (reaped == ankiPid) {
      // Child exited — clear the stored PID so we'll restart it.
      printf("[Camera] mm-anki-camera (pid %d) reaped, status=%d\n", (int)ankiPid, zombieStatus);
      fflush(stdout);
      gAnkiCameraPid.store(-1);
    }
  }

  pid_t existing = gCameraDaemonPid.load();
  if (existing > 0 && !processMatchesCommNonZombie(existing, "mm-qcamera-daem")) existing = -1;
  if (existing <= 0) existing = findProcessByComm("mm-qcamera-daem");
  if (existing > 0) {
    gCameraDaemonPid.store(existing);
  } else {
    // Try to locate the Qualcomm camera daemon binary.
    // Path varies by rootfs; check common locations.
    const char* bins[] = {
      "/usr/bin/mm-qcamera-daemon",
      "/system/bin/mm-qcamera-daemon",
      nullptr
    };
    const char* bin = nullptr;
    for (int i = 0; bins[i]; ++i) {
      if (exists(bins[i])) { bin = bins[i]; break; }
    }
    if (!bin) {
      error = "mm-qcamera-daemon not found in /usr/bin or /system/bin";
      return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
      error = "fork failed: " + std::string(strerror(errno));
      return false;
    }
    if (pid == 0) {
      int devNull = open("/dev/null", O_RDWR);
      if (devNull >= 0) {
        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        close(devNull);
      }
      execl(bin, bin, nullptr);
      _exit(1);
    }
    gCameraDaemonPid.store(pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
  }

  pid_t existingAnki = gAnkiCameraPid.load();
  if (existingAnki > 0 &&
      !processMatchesCommNonZombie(existingAnki, "mm-anki-camera") &&
      !processMatchesCommNonZombie(existingAnki, "mm-anki-camera-")) {
    existingAnki = -1;
  }
  if (existingAnki <= 0) {
    existingAnki = findProcessByComm("mm-anki-camera");
    if (existingAnki <= 0) existingAnki = findProcessByComm("mm-anki-camera-");
  }
  if (exists(kAnkiCameraSocket) && existingAnki > 0) {
    gAnkiCameraPid.store(existingAnki);
    return true;
  }
  if (exists(kAnkiCameraSocket) && existingAnki <= 0) {
    unlink(kAnkiCameraSocket);
  }

  const char* wrappers[] = {
    "/usr/bin/mm-anki-camera-wrapper",
    "/system/bin/mm-anki-camera-wrapper",
    "/usr/bin/mm-anki-camera",
    "/system/bin/mm-anki-camera",
    nullptr
  };
  const char* wrapper = nullptr;
  for (int i = 0; wrappers[i]; ++i) {
    if (exists(wrappers[i])) { wrapper = wrappers[i]; break; }
  }
  if (!wrapper) {
    error = "mm-anki-camera-wrapper not found in /usr/bin or /system/bin";
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    error = "fork mm-anki-camera failed: " + std::string(strerror(errno));
    return false;
  }
  if (pid == 0) {
    int devNull = open("/dev/null", O_RDWR);
    if (devNull >= 0) {
      dup2(devNull, STDIN_FILENO);
      dup2(devNull, STDOUT_FILENO);
      dup2(devNull, STDERR_FILENO);
      close(devNull);
    }
    // The API now owns the camera client lifecycle: register, start, heartbeat,
    // and format selection are sent over the camera IPC socket.
    execl(wrapper, wrapper, "-v", "0", "-r", "1", nullptr);
    _exit(1);
  }
  gAnkiCameraPid.store(pid);

  for (int i = 0; i < 30; ++i) {
    if (exists(kAnkiCameraSocket)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  error = "mm-anki-camera socket did not appear";
  return false;
}

void stopCameraDaemon() {
  pid_t pid = gCameraDaemonPid.exchange(-1);
  if (pid > 0) {
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, WNOHANG);
  }
  // Also kill by name in case we didn't spawn it
  runCommand("killall mm-qcamera-daemon >/dev/null 2>&1");
  runCommand("killall mm-anki-camera >/dev/null 2>&1");
  runCommand("killall mm-anki-camera-wrapper >/dev/null 2>&1");
}

void restartAnkiCameraProducer(const char* reason) {
  printf("[Camera] Restarting mm-anki-camera producer: %s\n", reason ? reason : "unknown");
  fflush(stdout);

  pid_t pid = gAnkiCameraPid.exchange(-1);
  if (pid > 0) {
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, WNOHANG);
  }
  reapExitedChildren();
  runCommand("killall -9 mm-anki-camera >/dev/null 2>&1");
  runCommand("killall -9 mm-anki-camera-wrapper >/dev/null 2>&1");
  reapExitedChildren();
  unlink(kAnkiCameraSocket);
}

bool cameraSnapshotAge(long& ageMs) {
  struct stat st;
  if (stat(kCameraSnapshotPath, &st) != 0) return false;
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  ageMs = (now.tv_sec - st.st_mtim.tv_sec) * 1000 +
          (now.tv_nsec - st.st_mtim.tv_nsec) / 1000000;
  return true;
}

bool receiveFdMessage(int sock, uint8_t* data, size_t dataLen, ssize_t& receivedLen, int& receivedFd) {
  char ctrl[CMSG_SPACE(sizeof(int) * 4)];
  iovec iov{};
  iov.iov_base = data;
  iov.iov_len = dataLen;
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrl;
  msg.msg_controllen = sizeof(ctrl);

  receivedFd = -1;
  receivedLen = recvmsg(sock, &msg, 0);
  if (receivedLen <= 0) return false;

  for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int* fds = reinterpret_cast<int*>(CMSG_DATA(c));
      int count = static_cast<int>((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      if (count > 0) {
        receivedFd = fds[0];
        for (int i = 1; i < count; ++i) close(fds[i]);
      }
    }
  }
  return true;
}

bool receiveCameraMessage(int sock, AnkiCameraMsg& msgOut, int& receivedFd) {
  char ctrl[CMSG_SPACE(sizeof(int) * 4)];
  iovec iov{};
  iov.iov_base = &msgOut;
  iov.iov_len = sizeof(msgOut);
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrl;
  msg.msg_controllen = sizeof(ctrl);

  receivedFd = -1;
  std::memset(&msgOut, 0, sizeof(msgOut));
  ssize_t receivedLen = recvmsg(sock, &msg, MSG_DONTWAIT);
  if (receivedLen < 0) return false;
  if (receivedLen != static_cast<ssize_t>(sizeof(msgOut))) return false;

  for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int* fds = reinterpret_cast<int*>(CMSG_DATA(c));
      int count = static_cast<int>((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      if (count > 0) {
        receivedFd = fds[0];
        for (int i = 1; i < count; ++i) close(fds[i]);
      }
    }
  }
  return true;
}

bool sendCameraMessage(int sock, uint32_t msgId, const void* payload = nullptr, size_t payloadLen = 0) {
  AnkiCameraMsg msg{};
  msg.msg_id = msgId;
  if (payload && payloadLen > 0) {
    if (payloadLen > sizeof(msg.payload)) return false;
    std::memcpy(msg.payload, payload, payloadLen);
  }
  return writeAllFd(sock, &msg, sizeof(msg));
}

bool sendCameraFormatMessage(int sock, uint32_t format) {
  struct FormatPayload {
    uint32_t id;
    uint32_t format;
  } payload{kAnkiCameraParamsFormat, format};
  return sendCameraMessage(sock, kAnkiCameraMsgClientParams, &payload, sizeof(payload));
}

std::string makeBmpFromRgb888(const uint8_t* rgb, int width, int height, int stride) {
  if (!rgb || width <= 0 || height <= 0 || stride < width * 3) return "";
  const int rowBytes = ((width * 3 + 3) / 4) * 4;
  const uint32_t pixelBytes = static_cast<uint32_t>(rowBytes * height);
  std::string bmp;
  bmp.reserve(14 + 40 + pixelBytes);
  bmp.push_back('B');
  bmp.push_back('M');
  appendLe32(bmp, 14 + 40 + pixelBytes);
  appendLe16(bmp, 0);
  appendLe16(bmp, 0);
  appendLe32(bmp, 14 + 40);
  appendLe32(bmp, 40);
  appendLe32(bmp, static_cast<uint32_t>(width));
  appendLe32(bmp, static_cast<uint32_t>(-height));
  appendLe16(bmp, 1);
  appendLe16(bmp, 24);
  appendLe32(bmp, 0);
  appendLe32(bmp, pixelBytes);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 0);
  appendLe32(bmp, 0);

  std::string pad(rowBytes - width * 3, '\0');
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = rgb + y * stride;
    for (int x = 0; x < width; ++x) {
      const uint8_t* px = row + x * 3;
      bmp.push_back(static_cast<char>(px[2]));
      bmp.push_back(static_cast<char>(px[1]));
      bmp.push_back(static_cast<char>(px[0]));
    }
    bmp += pad;
  }
  return bmp;
}

void unpackRaw10ContinuousRow(const uint8_t* row, int width, std::vector<uint16_t>& out) {
  out.assign(width, 0);
  int x = 0;
  int si = 0;
  while (x + 4 <= width) {
    uint8_t b0 = row[si + 0];
    uint8_t b1 = row[si + 1];
    uint8_t b2 = row[si + 2];
    uint8_t b3 = row[si + 3];
    uint8_t b4 = row[si + 4];
    out[x + 0] = static_cast<uint16_t>(b0 | ((b1 & 0x03) << 8));
    out[x + 1] = static_cast<uint16_t>((b1 >> 2) | ((b2 & 0x0f) << 6));
    out[x + 2] = static_cast<uint16_t>((b2 >> 4) | ((b3 & 0x3f) << 4));
    out[x + 3] = static_cast<uint16_t>((b3 >> 6) | (b4 << 2));
    x += 4;
    si += 5;
  }
}

enum class BayerPattern {
  BGGR,
  GBRG,
  GRBG,
  RGGB
};

BayerPattern parseBayerPattern(const std::string& str, BayerPattern def = BayerPattern::GBRG) {
  if (str == "BGGR" || str == "bggr") return BayerPattern::BGGR;
  if (str == "GBRG" || str == "gbrg") return BayerPattern::GBRG;
  if (str == "GRBG" || str == "grbg") return BayerPattern::GRBG;
  if (str == "RGGB" || str == "rggb") return BayerPattern::RGGB;
  return def;
}

std::string makeColorBmpFromRaw10(const uint8_t* raw, int width, int height, int stride, BayerPattern pattern = BayerPattern::GBRG) {
  const int outW = width / 2;
  const int outH = height / 2;
  std::vector<uint16_t> row0;
  std::vector<uint16_t> row1;
  std::vector<uint16_t> r_chan(outW * outH);
  std::vector<uint16_t> g_chan(outW * outH);
  std::vector<uint16_t> b_chan(outW * outH);
  uint32_t histR[1024]{};
  uint32_t histG[1024]{};
  uint32_t histB[1024]{};

  for (int y = 0; y < outH; ++y) {
    unpackRaw10ContinuousRow(raw + (y * 2) * stride, width, row0);
    unpackRaw10ContinuousRow(raw + (y * 2 + 1) * stride, width, row1);
    for (int x = 0; x < outW; ++x) {
      uint16_t b = 0, g = 0, r = 0;
      switch (pattern) {
        case BayerPattern::BGGR:
          b = row0[x * 2];
          g = (row0[x * 2 + 1] + row1[x * 2]) / 2;
          r = row1[x * 2 + 1];
          break;
        case BayerPattern::GBRG:
          g = (row0[x * 2] + row1[x * 2 + 1]) / 2;
          b = row0[x * 2 + 1];
          r = row1[x * 2];
          break;
        case BayerPattern::GRBG:
          g = (row0[x * 2] + row1[x * 2 + 1]) / 2;
          r = row0[x * 2 + 1];
          b = row1[x * 2];
          break;
        case BayerPattern::RGGB:
          r = row0[x * 2];
          g = (row0[x * 2 + 1] + row1[x * 2]) / 2;
          b = row1[x * 2 + 1];
          break;
      }

      b = std::min<uint16_t>(b, 1023);
      g = std::min<uint16_t>(g, 1023);
      r = std::min<uint16_t>(r, 1023);

      int idx = y * outW + x;
      r_chan[idx] = r;
      g_chan[idx] = g;
      b_chan[idx] = b;

      histR[r]++;
      histG[g]++;
      histB[b]++;
    }
  }

  const uint32_t total = static_cast<uint32_t>(outW * outH);
  const uint32_t lowCut = total / 150;
  const uint32_t highCut = total - lowCut;

  auto getBounds = [&](const uint32_t* hist, int& low, int& high) {
    uint32_t acc = 0;
    low = 0;
    high = 1023;
    for (int i = 0; i < 1024; ++i) {
      acc += hist[i];
      if (acc >= lowCut) { low = i; break; }
    }
    acc = 0;
    for (int i = 0; i < 1024; ++i) {
      acc += hist[i];
      if (acc >= highCut) { high = i; break; }
    }
    if (high <= low) high = low + 1;
  };

  int lowR, highR;
  int lowG, highG;
  int lowB, highB;
  getBounds(histR, lowR, highR);
  getBounds(histG, lowG, highG);
  getBounds(histB, lowB, highB);

  const int rowBytes = ((outW * 3 + 3) / 4) * 4;
  const uint32_t pixelBytes = static_cast<uint32_t>(rowBytes * outH);
  std::string bmp;
  bmp.reserve(14 + 40 + pixelBytes);
  bmp.push_back('B');
  bmp.push_back('M');
  appendLe32(bmp, 14 + 40 + pixelBytes);
  appendLe16(bmp, 0);
  appendLe16(bmp, 0);
  appendLe32(bmp, 14 + 40);
  appendLe32(bmp, 40);
  appendLe32(bmp, static_cast<uint32_t>(outW));
  appendLe32(bmp, static_cast<uint32_t>(-outH));
  appendLe16(bmp, 1);
  appendLe16(bmp, 24);
  appendLe32(bmp, 0);
  appendLe32(bmp, pixelBytes);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 0);
  appendLe32(bmp, 0);

  std::string pad(rowBytes - outW * 3, '\0');
  for (int y = 0; y < outH; ++y) {
    for (int x = 0; x < outW; ++x) {
      int idx = y * outW + x;
      int r = r_chan[idx];
      int g = g_chan[idx];
      int b = b_chan[idx];

      int scaledR = std::clamp((r - lowR) * 255 / (highR - lowR), 0, 255);
      int scaledG = std::clamp((g - lowG) * 255 / (highG - lowG), 0, 255);
      int scaledB = std::clamp((b - lowB) * 255 / (highB - lowB), 0, 255);

      bmp.push_back(static_cast<char>(scaledB));
      bmp.push_back(static_cast<char>(scaledG));
      bmp.push_back(static_cast<char>(scaledR));
    }
    bmp += pad;
  }
  return bmp;
}

std::string makeGrayBmpFromRaw10(const uint8_t* raw, int width, int height, int stride) {
  const int block = 4;
  const int outW = width / block;
  const int outH = height / block;
  std::vector<uint16_t> rows[block];
  std::vector<uint16_t> gray(outW * outH);
  uint32_t hist[1024]{};

  for (int y = 0; y < outH; ++y) {
    for (int r = 0; r < block; ++r) {
      unpackRaw10ContinuousRow(raw + (y * block + r) * stride, width, rows[r]);
    }
    for (int x = 0; x < outW; ++x) {
      uint32_t sum = 0;
      for (int r = 0; r < block; ++r) {
        for (int c = 0; c < block; ++c) {
          sum += rows[r][x * block + c];
        }
      }
      uint16_t v = static_cast<uint16_t>(sum / (block * block));
      v = std::min<uint16_t>(v, 1023);
      gray[y * outW + x] = v;
      hist[v]++;
    }
  }

  const uint32_t total = static_cast<uint32_t>(outW * outH);
  const uint32_t lowCut = total / 150;
  const uint32_t highCut = total - lowCut;
  uint32_t acc = 0;
  int low = 0;
  int high = 1023;
  for (int i = 0; i < 1024; ++i) {
    acc += hist[i];
    if (acc >= lowCut) { low = i; break; }
  }
  acc = 0;
  for (int i = 0; i < 1024; ++i) {
    acc += hist[i];
    if (acc >= highCut) { high = i; break; }
  }
  if (high <= low) high = low + 1;

  const int rowBytes = ((outW * 3 + 3) / 4) * 4;
  const uint32_t pixelBytes = static_cast<uint32_t>(rowBytes * outH);
  std::string bmp;
  bmp.reserve(14 + 40 + pixelBytes);
  bmp.push_back('B');
  bmp.push_back('M');
  appendLe32(bmp, 14 + 40 + pixelBytes);
  appendLe16(bmp, 0);
  appendLe16(bmp, 0);
  appendLe32(bmp, 14 + 40);
  appendLe32(bmp, 40);
  appendLe32(bmp, static_cast<uint32_t>(outW));
  appendLe32(bmp, static_cast<uint32_t>(-outH));
  appendLe16(bmp, 1);
  appendLe16(bmp, 24);
  appendLe32(bmp, 0);
  appendLe32(bmp, pixelBytes);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 0);
  appendLe32(bmp, 0);

  std::string pad(rowBytes - outW * 3, '\0');
  for (int y = 0; y < outH; ++y) {
    for (int x = 0; x < outW; ++x) {
      int scaled = std::clamp((gray[y * outW + x] - low) * 255 / (high - low), 0, 255);
      bmp.push_back(static_cast<char>(scaled));
      bmp.push_back(static_cast<char>(scaled));
      bmp.push_back(static_cast<char>(scaled));
    }
    bmp += pad;
  }
  return bmp;
}

std::thread gCameraThread;
std::atomic<bool> gCameraThreadStarted{false};
std::atomic<bool> gCameraEnabled{true};
std::mutex gCameraFrameMutex;
std::condition_variable gCameraFrameCond;
std::vector<uint8_t> gLatestRawFrameBytes;
int gLatestRawFrameWidth = 0;
int gLatestRawFrameHeight = 0;
int gLatestRawFrameStride = 0;
int gLatestRawFrameFormat = -1;
uint32_t gLatestRawFrameNum = 0;
uint64_t gLatestCameraSeq = 0;
std::string gLatestDefaultBmpBytes;

void cameraThreadLoop() {
  printf("[Camera] Thread started\n");
  fflush(stdout);

  int sock = -1;
  void* mapped = nullptr;
  size_t mapLen = 0;
  int mappedFd = -1;
  uint32_t lastFrameNum = 0;
  auto lastFrameAt = std::chrono::steady_clock::now();
  auto lastHeartbeatAt = std::chrono::steady_clock::now();
  bool registered = false;
  bool started = false;
  bool requestedRgb = false;
  char localPath[108] = {};

  auto lockAllSlots = [&]() {
    if (!mapped || mapLen < 64) return;
    auto* hdr = reinterpret_cast<uint32_t*>(mapped);
    if (hdr[0] != 0x304d4143) return;
    uint32_t slotCount = std::min<uint32_t>(hdr[8], kAnkiCameraMaxFrames);
    for (uint32_t slot = 0; slot < slotCount; ++slot) {
      uint32_t unlocked = 0;
      __atomic_compare_exchange_n(&hdr[2 + slot], &unlocked, 1, false,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
  };

  auto unlockAllSlots = [&]() {
    if (!mapped || mapLen < 64) return;
    auto* hdr = reinterpret_cast<uint32_t*>(mapped);
    if (hdr[0] != 0x304d4143) return;
    for (uint32_t slot = 0; slot < kAnkiCameraMaxFrames; ++slot) {
      uint32_t locked = 1;
      __atomic_compare_exchange_n(&hdr[2 + slot], &locked, 0, false,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
  };

  auto closeSock = [&]() {
    unlockAllSlots();
    if (sock >= 0) {
      sendCameraMessage(sock, kAnkiCameraMsgClientUnregister);
    }
    if (sock >= 0) { close(sock); sock = -1; }
    if (mappedFd >= 0) { close(mappedFd); mappedFd = -1; }
    if (localPath[0]) { unlink(localPath); localPath[0] = '\0'; }
    if (mapped) { munmap(mapped, mapLen); mapped = nullptr; }
    mapLen = 0;
    lastFrameNum = 0;
    lastFrameAt = std::chrono::steady_clock::now();
    lastHeartbeatAt = std::chrono::steady_clock::now();
    registered = false;
    started = false;
    requestedRgb = false;
  };

  while (gRunning) {
    if (!gCameraEnabled.load()) {
      closeSock();
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }

    if (sock < 0) {
      std::string error;
      if (!startCameraDaemon(error)) {
        printf("[Camera] Failed to start daemon: %s. Retrying in 1s...\n", error.c_str());
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      if (sock < 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      // CRITICAL: bind to a local path and KEEP IT ALIVE.
      // The camera server (mm-anki-camera) sends frame-ready notifications
      // back to this address via sendto(). If we unlink() the path, the
      // server's sendto() returns ENOENT, it concludes the client is dead,
      // stops streaming, and eventually exits. We must NOT unlink until we
      // are done and closing the socket.
      snprintf(localPath, sizeof(localPath), "/tmp/vector-camera-thread-%d-%ld.sock",
               getpid(), static_cast<long>(time(nullptr)));
      unlink(localPath);  // Remove any stale file from a previous crash.

      sockaddr_un local{};
      local.sun_family = AF_UNIX;
      snprintf(local.sun_path, sizeof(local.sun_path), "%s", localPath);
      if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        printf("[Camera] bind failed: %s\n", strerror(errno));
        fflush(stdout);
        close(sock); sock = -1;
        localPath[0] = '\0';
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      sockaddr_un server{};
      server.sun_family = AF_UNIX;
      snprintf(server.sun_path, sizeof(server.sun_path), "%s", kAnkiCameraSocket);

      if (connect(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) != 0) {
        printf("[Camera] connect failed: %s\n", strerror(errno));
        fflush(stdout);
        closeSock();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      if (!sendCameraMessage(sock, kAnkiCameraMsgClientRegister)) {
        printf("[Camera] client register failed: %s\n", strerror(errno));
        fflush(stdout);
        closeSock();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      registered = true;
      printf("[Camera] Registered camera client\n");
      fflush(stdout);
    }

    for (;;) {
      AnkiCameraMsg msg{};
      int receivedFd = -1;
      if (!receiveCameraMessage(sock, msg, receivedFd)) {
        if (receivedFd >= 0) close(receivedFd);
        break;
      }

      if (msg.msg_id == kAnkiCameraMsgServerBuffer && receivedFd >= 0) {
        uint32_t newMapLen = 0;
        std::memcpy(&newMapLen, msg.payload, sizeof(newMapLen));
        if (newMapLen == 0 || newMapLen > 64 * 1024 * 1024) {
          close(receivedFd);
          continue;
        }
        unlockAllSlots();
        if (mapped) {
          munmap(mapped, mapLen);
          mapped = nullptr;
        }
        if (mappedFd >= 0) close(mappedFd);
        mapLen = newMapLen;
        mappedFd = receivedFd;
        mapped = mmap(nullptr, mapLen, PROT_READ | PROT_WRITE, MAP_SHARED, mappedFd, 0);
        if (mapped == MAP_FAILED) {
          printf("[Camera] mmap failed: %s\n", strerror(errno));
          fflush(stdout);
          mapped = nullptr;
          closeSock();
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          break;
        }
        lastFrameNum = 0;
        lastFrameAt = std::chrono::steady_clock::now();
        printf("[Camera] Mapped camera buffer: %zu bytes\n", mapLen);
        fflush(stdout);
      } else if (msg.msg_id == kAnkiCameraMsgServerStatus) {
        uint32_t ack = msg.payload[0];
        if (ack == kAnkiCameraMsgClientRegister && registered && !started) {
          if (sendCameraMessage(sock, kAnkiCameraMsgClientStart)) {
            started = true;
            printf("[Camera] Start requested\n");
            fflush(stdout);
          }
        } else if (ack == kAnkiCameraMsgClientStart && !requestedRgb) {
          lockAllSlots();
          if (sendCameraFormatMessage(sock, kAnkiCameraFormatRgb888)) {
            requestedRgb = true;
            printf("[Camera] RGB888 format requested\n");
            fflush(stdout);
          }
        }
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (now - lastHeartbeatAt > std::chrono::milliseconds(200)) {
      if (!sendCameraMessage(sock, kAnkiCameraMsgClientHeartbeat)) {
        printf("[Camera] heartbeat failed: %s\n", strerror(errno));
        fflush(stdout);
        closeSock();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      lastHeartbeatAt = now;
    }

    if (!mapped) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    uint8_t* mem = static_cast<uint8_t*>(mapped);
    uint32_t* hdr = reinterpret_cast<uint32_t*>(mem);
    if (hdr[0] != 0x304d4143) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    uint32_t slotCount = std::min<uint32_t>(hdr[8], kAnkiCameraMaxFrames);
    if (slotCount == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    int bestSlot = -1;
    uint64_t bestTimestamp = 0;
    uint32_t bestFrame = 0;

    for (uint32_t slot = 0; slot < slotCount; ++slot) {
      uint32_t unlocked = 0;
      if (!__atomic_compare_exchange_n(&hdr[2 + slot], &unlocked, 1, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        continue;
      }

      uint32_t frameOffset = hdr[10 + slot];
      if (frameOffset + sizeof(AnkiCameraFrame) <= mapLen) {
        auto* frame = reinterpret_cast<AnkiCameraFrame*>(mem + frameOffset);
        uint64_t timestamp = frame->timestamp;
        if (timestamp != 0 && timestamp >= bestTimestamp) {
          bestTimestamp = timestamp;
          bestFrame = frame->frame_id;
          bestSlot = static_cast<int>(slot);
        }
      }
    }

    for (uint32_t slot = 0; slot < slotCount; ++slot) {
      if (static_cast<int>(slot) == bestSlot) continue;
      uint32_t locked = 1;
      __atomic_compare_exchange_n(&hdr[2 + slot], &locked, 0, false,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    if (bestSlot < 0 || bestFrame == 0 || bestFrame == lastFrameNum) {
      if (bestSlot >= 0) {
        uint32_t locked = 1;
        __atomic_compare_exchange_n(&hdr[2 + bestSlot], &locked, 0, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
      }
      if (now - lastFrameAt > std::chrono::seconds(5)) {
        printf("[Camera] No new camera frame for 5s, reconnecting client\n");
        fflush(stdout);
        closeSock();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      continue;
    }

    lastFrameNum = bestFrame;
    lastFrameAt = std::chrono::steady_clock::now();

    uint32_t frameOffset = hdr[10 + bestSlot];
    auto* cameraFrame = reinterpret_cast<AnkiCameraFrame*>(mem + frameOffset);
    uint32_t pixelOffset = frameOffset + sizeof(AnkiCameraFrame);
    int width = static_cast<int>(cameraFrame->width);
    int height = static_cast<int>(cameraFrame->height);
    int stride = static_cast<int>(cameraFrame->bytes_per_row);
    uint8_t bpp = cameraFrame->bits_per_pixel;
    uint8_t format = cameraFrame->format;

    std::string bmp;
    const size_t frameBytes = (width > 0 && height > 0 && stride > 0)
      ? static_cast<size_t>(height) * static_cast<size_t>(stride)
      : 0;
    if (frameBytes > 0 && static_cast<size_t>(pixelOffset) + frameBytes <= mapLen) {
      if (format == kAnkiCameraFormatRgb888 && bpp == 8) {
        bmp = makeBmpFromRgb888(mem + pixelOffset, width, height, stride);
        if (!bmp.empty()) {
          unlockAllSlots(); // Fix: unlock format-switch slots so mm-anki-camera can continue producing frames!
        }
      } else if (!requestedRgb) {
        lockAllSlots();
        sendCameraFormatMessage(sock, kAnkiCameraFormatRgb888);
        requestedRgb = true;
      }
      if (!bmp.empty()) {
        {
          std::lock_guard<std::mutex> lock(gCameraFrameMutex);
          gLatestRawFrameBytes.assign(mem + pixelOffset, mem + pixelOffset + frameBytes);
          gLatestRawFrameWidth = width;
          gLatestRawFrameHeight = height;
          gLatestRawFrameStride = stride;
          gLatestRawFrameFormat = format;
          gLatestRawFrameNum = bestFrame;
          gLatestDefaultBmpBytes = bmp;
          ++gLatestCameraSeq;
        }
        gCameraFrameCond.notify_all();
        writeWholeFile(kCameraSnapshotPath, bmp, 0644);
      }
    }

    uint32_t locked = 1;
    __atomic_compare_exchange_n(&hdr[2 + bestSlot], &locked, 0, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  closeSock();
}

void ensureCameraThreadStarted() {
  bool expected = false;
  if (gCameraThreadStarted.compare_exchange_strong(expected, true)) {
    gCameraThread = std::thread(cameraThreadLoop);
  }
}

// ── Motor position control ───────────────────────────────────────────────────
struct MotorPosCmd {
  int motor;       // 0=left, 1=right, 2=lift, 3=head
  int32_t ticks;   // signed target delta ticks
  double  power;   // 0.0-1.0 magnitude
  double  minPower;
  int     tolerance;
  int     timeoutMs;
};

struct TrackDriveCmd {
  int32_t ticks;    // positive = forward, negative = reverse
  double  power;    // 0.0-1.0 magnitude
  double  minPower;
  int     tolerance;
  int     timeoutMs;
};

constexpr int kEncoderSignForPositivePower[4] = {
  1,  // left track: positive power increases encoder ticks
  -1, // right track: positive API power moves forward and decreases raw encoder ticks
  1,  // lift
  1,  // head
};

double powerForEncoderError(int motor, double encoderControl) {
  if (motor < 0 || motor >= 4) return 0.0;
  return encoderControl * static_cast<double>(kEncoderSignForPositivePower[motor]);
}

double slewTowards(double current, double target, double step) {
  if (std::abs(target - current) <= step) return target;
  return current + (target > current ? step : -step);
}

double shapedPowerForError(int32_t error, double maxPower, double minPower, double kp) {
  if (error == 0) return 0.0;
  const double magnitude = std::clamp(std::abs(static_cast<double>(error)) * kp,
                                      std::abs(minPower), std::abs(maxPower));
  return (error > 0 ? 1.0 : -1.0) * magnitude;
}

// Forward-declare gSpine so we can use it in the position thread.
class Spine;
extern Spine gSpine;  // defined below

// Atomic flags per motor to cancel in-progress position commands.
std::atomic<bool> gMotorPosCancel[4]{ {false}, {false}, {false}, {false} };
std::atomic<bool> gTrackDriveCancel{false};

bool setAudioVolumePercent(int percent) {
  percent = std::clamp(percent, 0, 100);
  gAudioVolumePercent.store(percent);
  int mixer = std::clamp((kAudioMixerMax * percent + 50) / 100, 0, kAudioMixerMax);
  return runCommand("amixer cset numid=33 " + std::to_string(mixer) +
                    " >/tmp/vector-hw-volume.log 2>&1") == 0;
}

std::atomic<bool> gAudioStreamActive{false};
std::mutex gAudioStreamMutex;
int gAudioStreamSocket = -1;
sockaddr_in gAudioStreamAddr{};
uint32_t gAudioStreamSeq = 0;

void sendAudioUdp(const int16_t* audio, uint32_t spineFramecounter) {
  std::lock_guard<std::mutex> lock(gAudioStreamMutex);
  if (gAudioStreamSocket < 0) return;

  #pragma pack(push, 1)
  struct AudioHeader {
    uint32_t magic;
    uint32_t sequence;
    uint32_t timestamp;
    uint16_t channels;
    uint16_t sample_rate;
    uint32_t payload_len;
  };
  #pragma pack(pop)

  AudioHeader hdr{
    0x56415544, // "VAUD"
    gAudioStreamSeq++,
    spineFramecounter,
    4,
    16000,
    640
  };

  uint8_t packet[sizeof(AudioHeader) + 640];
  memcpy(packet, &hdr, sizeof(hdr));
  memcpy(packet + sizeof(hdr), audio, 640);

  sendto(gAudioStreamSocket, packet, sizeof(packet), 0,
         reinterpret_cast<const sockaddr*>(&gAudioStreamAddr), sizeof(gAudioStreamAddr));
}

bool startAudioStream(const std::string& ip, int port, std::string& error) {
  std::lock_guard<std::mutex> lock(gAudioStreamMutex);
  if (gAudioStreamSocket >= 0) {
    close(gAudioStreamSocket);
    gAudioStreamSocket = -1;
  }

  gAudioStreamSocket = socket(AF_INET, SOCK_DGRAM | O_CLOEXEC, 0);
  if (gAudioStreamSocket < 0) {
    error = "failed to create UDP socket: " + std::string(strerror(errno));
    return false;
  }

  gAudioStreamAddr = {};
  gAudioStreamAddr.sin_family = AF_INET;
  gAudioStreamAddr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &gAudioStreamAddr.sin_addr) <= 0) {
    error = "invalid destination IP address: " + ip;
    close(gAudioStreamSocket);
    gAudioStreamSocket = -1;
    return false;
  }

  gAudioStreamSeq = 0;
  gAudioStreamActive.store(true);
  return true;
}

void stopAudioStream() {
  std::lock_guard<std::mutex> lock(gAudioStreamMutex);
  gAudioStreamActive.store(false);
  if (gAudioStreamSocket >= 0) {
    close(gAudioStreamSocket);
    gAudioStreamSocket = -1;
  }
}

class Imu {
 public:
  bool init() {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ >= 0) return true;

    // Ensure the sensor is powered by enabling regulator 8916_l10
    // Try both 8916_l10 and pm8909_l10 under /sys/kernel/debug/regulator/
    bool regulatorEnabled = false;
    for (const char* path : {
        "/sys/kernel/debug/regulator/8916_l10/enable",
        "/sys/kernel/debug/regulator/pm8909_l10/enable",
        "/sys/kernel/debug/regulator/pm8909_regulator-l10/enable"
    }) {
      if (exists(path)) {
        if (writeTextFile(path, "1")) {
          printf("[IMU] Enabled regulator via %s\n", path);
          fflush(stdout);
          regulatorEnabled = true;
          break;
        }
      }
    }
    if (!regulatorEnabled) {
      // If debugfs regulator path is not found, try mounting debugfs first!
      runCommand("mount -t debugfs none /sys/kernel/debug >/dev/null 2>&1");
      // Try again
      for (const char* path : {
          "/sys/kernel/debug/regulator/8916_l10/enable",
          "/sys/kernel/debug/regulator/pm8909_l10/enable"
      }) {
        if (exists(path)) {
          if (writeTextFile(path, "1")) {
            printf("[IMU] Enabled regulator after mounting debugfs via %s\n", path);
            fflush(stdout);
            regulatorEnabled = true;
            break;
          }
        }
      }
    }
    // Give some time for power to stabilize
    usleep(50000);

    fd_ = open(kImuDevice, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
      lastError_ = "imu spidev unavailable: " + std::string(strerror(errno));
      return false;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = 15000000;

    // Configure BOTH SPI Write and Read settings to ensure symmetric phase and speed clocking
    if (ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd_, SPI_IOC_RD_MODE, &mode) < 0 ||
        ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd_, SPI_IOC_RD_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
        ioctl(fd_, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0) {
      lastError_ = "failed to configure imu spi settings";
      close(fd_);
      fd_ = -1;
      return false;
    }

    uint8_t whoami = 0;
    // BMI160 starts in I2C mode after power-up. Bosch recommends one dummy
    // SPI read from 0x7f before normal register access to switch to SPI.
    uint8_t dummy = 0;
    readRegisterLocked(0x7F, &dummy, 1);

    if (!readRegisterLocked(0x00, &whoami, 1)) {
      lastError_ = "failed to read BMI160 CHIP_ID register";
      close(fd_);
      fd_ = -1;
      return false;
    }

    if (whoami != 0xD1) {
      char buf[96];
      snprintf(buf, sizeof(buf), "unexpected BMI160 CHIP_ID 0x%02X, expected 0xD1", whoami);
      lastError_ = buf;
      close(fd_);
      fd_ = -1;
      return false;
    }

    whoami_ = whoami;
    printf("[IMU] SPI configuration successful. Detected BMI160 CHIP_ID = 0x%02X\n", whoami);
    fflush(stdout);

    // Power up accelerometer and gyroscope, then configure the same ranges and
    // output rate used by Anki's original robot HAL.
    if (!writeRegisterLocked(0x7E, 0x11)) {
      lastError_ = "failed to power up BMI160 accelerometer";
      close(fd_);
      fd_ = -1;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    if (!writeRegisterLocked(0x7E, 0x15)) {
      lastError_ = "failed to power up BMI160 gyroscope";
      close(fd_);
      fd_ = -1;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(85));

    if (!writeRegisterLocked(0x41, 0x03) ||  // ACC_RANGE: +/-2g
        !writeRegisterLocked(0x40, 0x19) ||  // ACC_CONF: 200Hz
        !writeRegisterLocked(0x43, 0x02) ||  // GYR_RANGE: +/-500dps
        !writeRegisterLocked(0x42, 0x09) ||  // GYR_CONF: 200Hz, normal filter
        !writeRegisterLocked(0x53, 0x44)) {  // INT_OUT_CTRL: open-drain, disabled
      lastError_ = "failed to configure BMI160 ranges/rates/interrupts";
      close(fd_);
      fd_ = -1;
      return false;
    }

    initialized_ = true;
    lastError_.clear();

    running_ = true;
    pollThread_ = std::thread([this] { pollLoop(); });
    return true;
  }

  void stop() {
    running_ = false;
    if (pollThread_.joinable()) pollThread_.join();
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    initialized_ = false;
  }

  bool initialized() const { return initialized_; }
  uint8_t whoami() const { return whoami_; }
  std::string lastError() const { return lastError_; }

  void getRaw(double& ax, double& ay, double& az, double& gx, double& gy, double& gz) {
    std::lock_guard<std::mutex> lock(mu_);
    ax = ax_; ay = ay_; az = az_;
    gx = gx_; gy = gy_; gz = gz_;
  }

  double getTemp() {
    std::lock_guard<std::mutex> lock(mu_);
    return temp_;
  }

 private:
  bool readRegisterLocked(uint8_t reg, uint8_t* val, size_t len) {
    if (fd_ < 0) return false;

    std::vector<uint8_t> tx(len + 1, 0);
    std::vector<uint8_t> rx(len + 1, 0);
    tx[0] = reg | 0x80;

    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<unsigned long>(tx.data());
    tr.rx_buf = reinterpret_cast<unsigned long>(rx.data());
    tr.len = len + 1;
    tr.speed_hz = 15000000;
    tr.bits_per_word = 8;

    if (ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) < 0) return false;

    memcpy(val, rx.data() + 1, len);
    return true;
  }

  bool writeRegisterLocked(uint8_t reg, uint8_t val) {
    if (fd_ < 0) return false;

    uint8_t tx[2] = { reg, val };
    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<unsigned long>(tx);
    tr.len = 2;
    tr.speed_hz = 15000000;
    tr.bits_per_word = 8;

    return ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) >= 0;
  }

  void pollLoop() {
    uint8_t data[12];
    int diagCounter = 0;
    while (running_) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (fd_ >= 0) {
          if (readRegisterLocked(0x0C, data, 12)) {
            const auto le16 = [](uint8_t lo, uint8_t hi) {
              return static_cast<int16_t>((static_cast<uint16_t>(hi) << 8) | lo);
            };

            // BMI160 raw order is gyro xyz, accel xyz. Keep Anki's original
            // Vector HAL axis mapping so API values match the robot frame.
            int16_t raw_gx = le16(data[4], data[5]);
            int16_t raw_gy = le16(data[2], data[3]);
            int16_t raw_gz = -le16(data[0], data[1]);
            int16_t raw_ax = le16(data[10], data[11]);
            int16_t raw_ay = le16(data[8], data[9]);
            int16_t raw_az = -le16(data[6], data[7]);

            ax_ = static_cast<double>(raw_ax) * 2.0 / 32767.0;
            ay_ = static_cast<double>(raw_ay) * 2.0 / 32767.0;
            az_ = static_cast<double>(raw_az) * 2.0 / 32767.0;

            gx_ = static_cast<double>(raw_gx) * 500.0 / 32767.0;
            gy_ = static_cast<double>(raw_gy) * 500.0 / 32767.0;
            gz_ = static_cast<double>(raw_gz) * 500.0 / 32767.0;

            // Periodically read temperature (every 50 polls, i.e., once per second)
            if (diagCounter == 0) {
              uint8_t temp_data[2];
              if (readRegisterLocked(0x20, temp_data, 2)) {
                int16_t raw_temp = static_cast<int16_t>((static_cast<uint16_t>(temp_data[1]) << 8) | temp_data[0]);
                // BMI160 temperature formula: raw * (64.0 / 32768.0) + 23.0
                temp_ = static_cast<double>(raw_temp) * 64.0 / 32768.0 + 23.0;
              }
            }

            // Print diagnostics once per second (every 50 polls at 20ms)
            if (++diagCounter >= 50) {
              diagCounter = 0;
              printf("[IMU] bmi160 raw: ax=%d ay=%d az=%d gx=%d gy=%d gz=%d temp=%.2fC "
                     "bytes: %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x\n",
                     raw_ax, raw_ay, raw_az, raw_gx, raw_gy, raw_gz, temp_,
                     data[0], data[1], data[2], data[3], data[4], data[5],
                     data[6], data[7], data[8], data[9], data[10], data[11]);
              fflush(stdout);
            }
          } else {
            printf("[IMU] SPI read failed at BMI160 data registers\n");
            fflush(stdout);
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  mutable std::mutex mu_;
  int fd_ = -1;
  bool initialized_ = false;
  uint8_t whoami_ = 0;
  std::string lastError_;
  std::atomic<bool> running_{false};
  std::thread pollThread_;

  double ax_ = 0, ay_ = 0, az_ = 0;
  double gx_ = 0, gy_ = 0, gz_ = 0;
  double temp_ = 0;
};

Imu gImu;

void configureSpineSerial(int fd) {
  termios cfg{};
  if (tcgetattr(fd, &cfg) == 0) {
    cfmakeraw(&cfg);
    cfsetispeed(&cfg, B3000000);
    cfsetospeed(&cfg, B3000000);
    cfg.c_cflag &= ~CSIZE;
    cfg.c_cflag |= CS8 | CSTOPB | CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &cfg);
    tcflush(fd, TCIOFLUSH);
  }
}

bool sendStopMotorsOnce() {
  int fd = open(kSpineDevice, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0) return false;
  configureSpineSerial(fd);
  HeadToBody head{};
  SpineHeader hdr{kSyncHeadToBody, kPayloadDataFrame, static_cast<uint16_t>(sizeof(HeadToBody))};
  const uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(&head), sizeof(head));
  bool ok = true;
  for (int i = 0; i < 3; ++i) {
    ok = writeAllFd(fd, &hdr, sizeof(hdr)) && ok;
    ok = writeAllFd(fd, &head, sizeof(head)) && ok;
    ok = writeAllFd(fd, &crc, sizeof(crc)) && ok;
    usleep(20000);
  }
  close(fd);
  return ok;
}

class GpioPin {
 public:
  bool openPin(int number, bool output, bool initialHigh, bool openDrain = false) {
    closePin();
    number_ = number;
    openDrain_ = openDrain;
    sysfs_ = "/sys/class/gpio/gpio" + std::to_string(gpioNumber());
    if (!exists(sysfs_)) {
      writeTextFile("/sys/class/gpio/export", std::to_string(gpioNumber()));
      for (int i = 0; i < 20 && !exists(sysfs_); ++i) usleep(10000);
    }
    if (!setDirection(output)) return false;
    fd_ = open((sysfs_ + "/value").c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) return false;
    return set(initialHigh);
  }

  bool set(bool high) {
    if (fd_ < 0) return false;
    if (openDrain_) return setDirection(!high);
    const char* value = high ? "1" : "0";
    return pwrite(fd_, value, 1, 0) == 1;
  }

  void closePin() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
  }

 private:
  static int gpioBase() {
    static int base = -2;
    if (base != -2) return base;
    for (const char* path : {"/sys/devices/platform/soc/1000000.pinctrl/gpio/gpiochip0/base",
                             "/sys/devices/soc/1000000.pinctrl/gpio/gpiochip0/base"}) {
      std::string data = readFile(path, 32);
      if (!data.empty() && std::isdigit(static_cast<unsigned char>(data[0]))) {
        base = std::atoi(data.c_str());
        return base;
      }
    }
    base = 0;
    return base;
  }

  int gpioNumber() const {
    return number_ + gpioBase();
  }

  bool setDirection(bool output) {
    return writeTextFile(sysfs_ + "/direction", output ? "out" : "in");
  }

  int number_ = -1;
  int fd_ = -1;
  bool openDrain_ = false;
  std::string sysfs_;
};

struct LcdInitStep {
  uint8_t cmd;
  uint8_t dataBytes;
  uint8_t data[64];
  uint32_t delayMs;
};

enum class LcdPanel {
  Unknown,
  Santek,
  Midas,
};

class Lcd {
 public:
  bool available() const {
    return exists(kLcdDevice);
  }

  std::string panelName() const {
    switch (panel_) {
      case LcdPanel::Santek: return "santek";
      case LcdPanel::Midas: return "midas";
      default: return "unknown";
    }
  }

  bool initialized() const {
    return initialized_;
  }

  std::string lastError() const {
    return lastError_;
  }

  bool setBrightness(int level) {
    const char* lights[] = {"/sys/class/leds/face-backlight/brightness",
                            "/sys/class/leds/face-backlight-left/brightness",
                            "/sys/class/leds/face-backlight-right/brightness"};
    bool ok = false;
    for (const char* light : lights) {
      if (exists(light)) ok = writeTextFile(light, std::to_string(level)) || ok;
    }
    if (!ok) lastError_ = "no backlight sysfs nodes";
    return ok;
  }

  bool drawFrame(const std::string& body, bool autoInit = true) {
    std::lock_guard<std::mutex> lock(mu_);
    if (body.size() != kLcdBytes) {
      lastError_ = "expected exactly 184x96 RGB565 bytes";
      return false;
    }
    if (autoInit && !ensureInitializedLocked()) return false;
    if (spiFd_ < 0) {
      lastError_ = "lcd spidev unavailable";
      return false;
    }
    const uint8_t writeRam = 0x2c;
    if (!transferLocked(true, &writeRam, 1)) return false;
    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(body.data());
    if (panel_ == LcdPanel::Midas) {
      uint16_t row[kLcdMidasWidth];
      for (int y = 0; y < kLcdMidasHeight; ++y) {
        int srcY = y * kLcdHeight / kLcdMidasHeight;
        for (int x = 0; x < kLcdMidasWidth; ++x) {
          int srcX = x * kLcdWidth / kLcdMidasWidth;
          row[x] = bswap16(pixels[srcY * kLcdWidth + srcX]);
        }
        if (!transferLocked(false, row, sizeof(row))) return false;
      }
      return true;
    }
    return transferLocked(false, body.data(), body.size());
  }

  bool init() {
    std::lock_guard<std::mutex> lock(mu_);
    initialized_ = false;
    return ensureInitializedLocked();
  }

 private:
  static uint16_t bswap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
  }

  LcdPanel detectPanelLocked() {
    std::string emr = readFile("/dev/mmcblk0p29", 32);
    if (emr.size() >= 8) {
      uint32_t hw = 0;
      memcpy(&hw, emr.data() + 4, sizeof(hw));
      if (hw >= 0x20) return LcdPanel::Midas;
      if (hw > 0 && hw <= 7) return LcdPanel::Santek;
    }
    return LcdPanel::Santek;
  }

  bool ensureInitializedLocked() {
    if (initialized_) return true;
    panel_ = detectPanelLocked();
    if (!wrx_.openPin(kGpioLcdWrx, true, true)) {
      lastError_ = "cannot open LCD D/C GPIO";
      return false;
    }
    const bool reset2OpenDrain = panel_ != LcdPanel::Midas;
    if (!reset1_.openPin(kGpioLcdReset1, true, true, true) ||
        !reset2_.openPin(kGpioLcdReset2, true, true, reset2OpenDrain)) {
      lastError_ = "cannot open LCD reset GPIOs";
      return false;
    }
    spiFd_ = open(kLcdDevice, O_RDWR | O_CLOEXEC);
    if (spiFd_ < 0) {
      lastError_ = "lcd spidev unavailable";
      return false;
    }
    uint8_t mode = 0;
    uint8_t bits = 8;
    uint32_t speed = kLcdDataClock;
    ioctl(spiFd_, SPI_IOC_WR_MODE, &mode);
    ioctl(spiFd_, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spiFd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    resetLocked();
    runScriptLocked(panel_ == LcdPanel::Midas ? kInitMidas : kInitSantek);
    std::string blank(kLcdBytes, '\0');
    drawFrameLocked(blank);
    runScriptLocked(panel_ == LcdPanel::Midas ? kOnMidas : kOnSantek);
    initialized_ = true;
    lastError_.clear();
    return true;
  }

  void resetLocked() {
    usleep(50);
    reset1_.set(false);
    reset2_.set(false);
    usleep(50);
    reset1_.set(true);
    reset2_.set(true);
    usleep(50);
    const uint8_t sleepOut = 0x11;
    transferLocked(true, &sleepOut, 1);
    usleep(50);
  }

  bool runScriptLocked(const LcdInitStep* script) {
    for (int i = 0; script[i].cmd; ++i) {
      if (!transferLocked(true, &script[i].cmd, 1)) return false;
      if (script[i].dataBytes && !transferLocked(false, script[i].data, script[i].dataBytes)) return false;
      if (script[i].delayMs) usleep(script[i].delayMs * 1000);
    }
    return true;
  }

  bool drawFrameLocked(const std::string& frame) {
    const uint8_t writeRam = 0x2c;
    if (!transferLocked(true, &writeRam, 1)) return false;
    return transferLocked(false, frame.data(), frame.size());
  }

  bool transferLocked(bool command, const void* data, size_t len) {
    if (spiFd_ < 0) return false;
    wrx_.set(!command);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
      size_t n = std::min<size_t>(len, kLcdMaxTransfer);
      if (!writeAllFd(spiFd_, p, n)) {
        lastError_ = "lcd spi write failed";
        return false;
      }
      p += n;
      len -= n;
    }
    return true;
  }

  static const LcdInitStep kInitSantek[];
  static const LcdInitStep kInitMidas[];
  static const LcdInitStep kOnSantek[];
  static const LcdInitStep kOnMidas[];

  mutable std::mutex mu_;
  int spiFd_ = -1;
  bool initialized_ = false;
  LcdPanel panel_ = LcdPanel::Unknown;
  std::string lastError_;
  GpioPin wrx_;
  GpioPin reset1_;
  GpioPin reset2_;
};

const LcdInitStep Lcd::kInitSantek[] = {
  {0x10, 1, {0x00}, 120},
  {0x2a, 4, {0x00, 0x1c, 0x00, 0xd3}, 0},
  {0x2b, 4, {0x00, 0x00, 0x00, 0x5f}, 0},
  {0x36, 1, {0x00}, 0},
  {0x3a, 1, {0x55}, 0},
  {0xb0, 2, {0x00, 0x08}, 0},
  {0xb2, 5, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 0},
  {0xb7, 1, {0x72}, 0},
  {0xbb, 1, {0x3b}, 0},
  {0xc0, 1, {0x2c}, 0},
  {0xc2, 1, {0x01}, 0},
  {0xc3, 1, {0x14}, 0},
  {0xc4, 1, {0x20}, 0},
  {0xc6, 1, {0x0f}, 0},
  {0xd0, 2, {0xa4, 0xa1}, 0},
  {0xe0, 14, {0xd0, 0x10, 0x16, 0x0a, 0x0a, 0x26, 0x3c, 0x53, 0x53, 0x18, 0x15, 0x12, 0x36, 0x3c}, 0},
  {0xe1, 14, {0xd0, 0x11, 0x19, 0x0a, 0x09, 0x25, 0x3d, 0x35, 0x54, 0x17, 0x15, 0x12, 0x36, 0x3c}, 0},
  {0xe9, 3, {0x05, 0x05, 0x01}, 0},
  {0x21, 1, {0x00}, 0},
  {0, 0, {0}, 0},
};

const LcdInitStep Lcd::kInitMidas[] = {
  {0x01, 0, {0}, 150},
  {0x11, 0, {0}, 500},
  {0x20, 0, {0}, 0},
  {0x36, 1, {0xa8}, 0},
  {0x3a, 1, {0x05}, 0},
  {0xe0, 16, {0x07, 0x0e, 0x08, 0x07, 0x10, 0x07, 0x02, 0x07, 0x09, 0x0f, 0x25, 0x36, 0x00, 0x08, 0x04, 0x10}, 0},
  {0xe1, 16, {0x0a, 0x0d, 0x08, 0x07, 0x0f, 0x07, 0x02, 0x07, 0x09, 0x0f, 0x25, 0x35, 0x00, 0x09, 0x04, 0x10}, 0},
  {0xfc, 1, {0xc0}, 0},
  {0x13, 0, {0}, 100},
  {0x26, 1, {0x02}, 10},
  {0x29, 0, {0}, 10},
  {0x2a, 4, {0x00, 0x00, 0x00, 0x9f}, 0},
  {0x2b, 4, {0x00, 0x18, 0x00, 0x67}, 0},
  {0, 0, {0}, 0},
};

const LcdInitStep Lcd::kOnSantek[] = {
  {0x11, 1, {0x00}, 120},
  {0x29, 1, {0x00}, 120},
  {0, 0, {0}, 0},
};

const LcdInitStep Lcd::kOnMidas[] = {
  {0x11, 1, {0x00}, 120},
  {0x29, 1, {0x00}, 120},
  {0, 0, {0}, 0},
};

std::string httpDate() {
  char buf[128];
  time_t now = time(nullptr);
  tm tmNow{};
  gmtime_r(&now, &tmNow);
  strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tmNow);
  return buf;
}

std::string b64(const uint8_t* data, size_t len) {
  static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= data[i + 2];
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
    out += (i + 2 < len) ? tbl[n & 63] : '=';
  }
  return out;
}

bool sendAll(int fd, const std::string& data) {
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
    if (n <= 0) return false;
    p += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

bool sendChunk(int fd, const std::string& data) {
  if (data.empty()) return true;
  std::ostringstream out;
  out << std::hex << data.size() << "\r\n" << data << "\r\n";
  return sendAll(fd, out.str());
}

bool sendEndChunks(int fd) {
  return sendAll(fd, "0\r\n\r\n");
}

void sendResponse(int fd, int code, const std::string& reason, const std::string& body,
                  const std::string& contentType = "application/json") {
  std::ostringstream out;
  out << "HTTP/1.1 " << code << " " << reason << "\r\n";
  out << "Date: " << httpDate() << "\r\n";
  out << "Server: vector-hw-api/" << kApiVersion << "\r\n";
  out << "Connection: close\r\n";
  out << "Content-Type: " << contentType << "\r\n";
  out << "Content-Length: " << body.size() << "\r\n\r\n";
  out << body;
  sendAll(fd, out.str());
}

void sendJsonError(int fd, int code, const std::string& message) {
  sendResponse(fd, code, code == 404 ? "Not Found" : "Error",
               "{\"error\":\"" + jsonEscape(message) + "\"}");
}

double numberField(const std::string& body, const std::string& name, double fallback) {
  const std::string needle = "\"" + name + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) return fallback;
  p = body.find(':', p);
  if (p == std::string::npos) return fallback;
  ++p;
  while (p < body.size() && isspace(static_cast<unsigned char>(body[p]))) ++p;
  // Support boolean values true/false directly
  if (p + 4 <= body.size() && body.compare(p, 4, "true") == 0) {
    return 1.0;
  }
  if (p + 5 <= body.size() && body.compare(p, 5, "false") == 0) {
    return 0.0;
  }
  char* end = nullptr;
  double v = strtod(body.c_str() + p, &end);
  if (end == body.c_str() + p) return fallback;
  return v;
}

std::string stringField(const std::string& body, const std::string& name, const std::string& fallback) {
  const std::string needle = "\"" + name + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) return fallback;
  p = body.find(':', p);
  if (p == std::string::npos) return fallback;
  ++p;
  while (p < body.size() && (isspace(static_cast<unsigned char>(body[p])) || body[p] == '"')) ++p;
  size_t start = p;
  while (p < body.size() && body[p] != '"' && body[p] != ',' && body[p] != '}') ++p;
  size_t end = p;
  if (start >= end) return fallback;
  if (end > start && body[end-1] == '"') --end;
  if (start < end && body[start] == '"') ++start;
  return body.substr(start, end - start);
}

class Spine {
 public:
  void start() {
    reader_ = std::thread([this] { readLoop(); });
    writer_ = std::thread([this] { writeLoop(); });
  }

  void stop() {
    gRunning = false;
    zeroMotors();
    if (reader_.joinable()) reader_.join();
    if (writer_.joinable()) writer_.join();
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
  }

  bool available() const {
    return exists(kSpineDevice);
  }

  bool connected() const {
    return fd_ >= 0;
  }

  void setMotors(double left, double right, double lift, double head, int ttlMs) {
    std::lock_guard<std::mutex> lock(mu_);
    head_.motorPower[0] = scaleMotor(left);
    head_.motorPower[1] = scaleMotor(-right);
    head_.motorPower[2] = scaleMotor(lift);
    head_.motorPower[3] = scaleMotor(head);
    motorDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::clamp(ttlMs, 1, kMaxTtlMs));
    sendFrameLocked();
  }

  void zeroMotors() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& m : head_.motorPower) m = 0;
    motorDeadline_ = std::chrono::steady_clock::now();
    sendFrameLocked();
  }

  void setBackpack(const std::vector<uint8_t>& rgb) {
    std::lock_guard<std::mutex> lock(mu_);
    std::fill(std::begin(head_.lightState.ledColors), std::end(head_.lightState.ledColors), 0);
    // Square LEDs 0, 1, 2
    size_t limit = std::min<size_t>(rgb.size(), 9);
    for (size_t i = 0; i < limit; ++i) {
      head_.lightState.ledColors[i] = rgb[i];
    }
    // Status/Button LED 3
    if (rgb.size() >= 12) {
      head_.lightState.ledColors[9] = 255 - rgb[11];  // Physical Blue
      head_.lightState.ledColors[10] = 255 - rgb[10]; // Physical Green
      head_.lightState.ledColors[11] = 255 - rgb[9];  // Physical Red
    } else if (rgb.size() >= 10) {
      head_.lightState.ledColors[9] = 255;
      head_.lightState.ledColors[10] = 255;
      head_.lightState.ledColors[11] = 255;
    }
    sendFrameLocked();
  }

  void setBackpackLed(int led, uint8_t r, uint8_t g, uint8_t b) {
    if (led < 0 || led >= 4) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (led == 3) {
      uint8_t temp_r = 255 - r;
      uint8_t temp_g = 255 - g;
      uint8_t temp_b = 255 - b;
      head_.lightState.ledColors[9] = temp_b;  // Physical Blue
      head_.lightState.ledColors[10] = temp_g; // Physical Green
      head_.lightState.ledColors[11] = temp_r; // Physical Red
    } else {
      head_.lightState.ledColors[led * 3 + 0] = r;
      head_.lightState.ledColors[led * 3 + 1] = g;
      head_.lightState.ledColors[led * 3 + 2] = b;
    }
    sendFrameLocked();
  }

  BodyToHead snapshot(bool* valid) const {
    std::lock_guard<std::mutex> lock(mu_);
    *valid = bodyValid_;
    return body_;
  }

  bool backButtonPressed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return powerButtonPressed_;
  }

  bool powerButtonPressed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return powerButtonPressed_;
  }

  uint64_t powerButtonHoldMs() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!powerButtonPressed_ || powerButtonPressedAt_ == std::chrono::steady_clock::time_point{}) {
      return 0;
    }
    return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - powerButtonPressedAt_).count());
  }

 private:
  static int16_t scaleMotor(double v) {
    v = std::clamp(v, -1.0, 1.0);
    return static_cast<int16_t>(v * 32767.0);
  }

  bool openSerialLocked() {
    if (fd_ >= 0) return true;
    fd_ = open(kSpineDevice, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) return false;
    configureSpineSerial(fd_);
    lastFrameAt_ = std::chrono::steady_clock::now();
    return true;
  }

  void reopenSerialLocked(const char* reason) {
    if (fd_ >= 0) {
      printf("[Spine] Reopening %s: %s\n", kSpineDevice, reason ? reason : "unknown");
      fflush(stdout);
      close(fd_);
      fd_ = -1;
    }
    lastFrameAt_ = std::chrono::steady_clock::now();
  }

  void sendFrameLocked() {
    if (!openSerialLocked()) return;
    head_.framecounter++;
    SpineHeader hdr{kSyncHeadToBody, kPayloadDataFrame, static_cast<uint16_t>(sizeof(HeadToBody))};
    const uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(&head_), sizeof(head_));
    writeAllFd(fd_, &hdr, sizeof(hdr));
    writeAllFd(fd_, &head_, sizeof(head_));
    writeAllFd(fd_, &crc, sizeof(crc));
  }

  void writeLoop() {
    while (gRunning) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        if (now >= motorDeadline_) {
          // Motor TTL expired — zero out any leftover power before polling
          for (auto& m : head_.motorPower) m = 0;
        }
        // Always send a frame at 50 Hz so the Spine MCU keeps responding
        // and telemetry (cliffs, distance, touch) is continuously updated.
        sendFrameLocked();
        if (fd_ >= 0 && now - lastFrameAt_ > std::chrono::milliseconds(1500)) {
          reopenSerialLocked("no body frame for 1500ms");
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  void readLoop() {
    std::vector<uint8_t> buf;
    uint8_t tmp[512];
    while (gRunning) {
      int localFd = -1;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!openSerialLocked()) {
          localFd = -1;
        } else {
          localFd = fd_;
        }
      }
      if (localFd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      ssize_t n = read(localFd, tmp, sizeof(tmp));
      if (n > 0) {
        buf.insert(buf.end(), tmp, tmp + n);
        parseBuffer(buf);
      } else {
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          std::lock_guard<std::mutex> lock(mu_);
          close(fd_);
          fd_ = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }

  void parseBuffer(std::vector<uint8_t>& buf) {
    while (buf.size() >= sizeof(SpineHeader)) {
      size_t start = 0;
      bool found = false;
      for (; start + sizeof(uint32_t) <= buf.size(); ++start) {
        uint32_t sync;
        memcpy(&sync, buf.data() + start, sizeof(sync));
        if (sync == kSyncBodyToHead) {
          found = true;
          break;
        }
      }
      if (!found) {
        buf.clear();
        return;
      }
      if (start > 0) buf.erase(buf.begin(), buf.begin() + start);
      if (buf.size() < sizeof(SpineHeader)) return;
      SpineHeader hdr;
      memcpy(&hdr, buf.data(), sizeof(hdr));
      size_t payloadLen = 0;
      if (hdr.payloadType == kPayloadDataFrame) {
        payloadLen = sizeof(BodyToHead);
      } else if (hdr.payloadType == kPayloadBootFrame) {
        payloadLen = sizeof(MicroBodyToHead);
      } else {
        buf.erase(buf.begin());
        continue;
      }
      if (hdr.bytesToFollow != payloadLen) {
        buf.erase(buf.begin());
        continue;
      }
      const size_t frameLen = sizeof(SpineHeader) + payloadLen + sizeof(uint32_t);
      if (buf.size() < frameLen) return;
      uint32_t expected;
      const uint8_t* payload = buf.data() + sizeof(SpineHeader);
      memcpy(&expected, payload + payloadLen, sizeof(expected));
      if (crc32(payload, payloadLen) == expected) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          lastFrameAt_ = std::chrono::steady_clock::now();
          if (hdr.payloadType == kPayloadDataFrame) {
            BodyToHead body;
            memcpy(&body, payload, sizeof(body));
            body_ = body;
            bodyValid_ = true;
          } else if (hdr.payloadType == kPayloadBootFrame) {
            MicroBodyToHead micro;
            memcpy(&micro, payload, sizeof(micro));
            const bool pressed = micro.buttonPressed != 0;
            const auto now = std::chrono::steady_clock::now();
            if (pressed && !powerButtonPressed_) {
              powerButtonPressedAt_ = now;
            } else if (!pressed) {
              powerButtonPressedAt_ = {};
            }
            powerButtonPressed_ = pressed;
          }
        }
        if (hdr.payloadType == kPayloadDataFrame && gAudioStreamActive.load()) {
          BodyToHead body;
          memcpy(&body, payload, sizeof(body));
          sendAudioUdp(body.audio, body.framecounter);
        }
      }
      buf.erase(buf.begin(), buf.begin() + frameLen);
    }
  }

  mutable std::mutex mu_;
  int fd_ = -1;
  HeadToBody head_{};
  BodyToHead body_{};
  bool bodyValid_ = false;
  bool powerButtonPressed_ = false;
  std::chrono::steady_clock::time_point powerButtonPressedAt_{};
  std::chrono::steady_clock::time_point lastFrameAt_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point motorDeadline_ = std::chrono::steady_clock::now();
  std::thread reader_;
  std::thread writer_;
};

Spine gSpine;
Lcd gLcd;

// ── VVID Video Player Globals ──────────────────────────────────────
std::atomic<bool> gVvidPlaying{false};
std::thread gVvidThread;
std::string gVvidCurrentName;

void stopVvidPlaying() {
  if (gVvidPlaying.load()) {
    gVvidPlaying.store(false);
    runCommand("killall aplay >/dev/null 2>&1");
    if (gVvidThread.joinable()) {
      gVvidThread.join();
    }
  }
}

void vvidPlayThread(std::string filepath) {
  std::ifstream in(filepath, std::ios::binary);
  if (!in) {
    gVvidPlaying.store(false);
    return;
  }

  // 1. Read header
  char magic[4];
  in.read(magic, 4);
  if (std::memcmp(magic, "VVID", 4) != 0) {
    gVvidPlaying.store(false);
    return;
  }

  uint32_t version = 0;
  uint32_t fps = 0;
  uint32_t frameCount = 0;
  uint32_t audioBytes = 0;
  uint32_t videoOffset = 0;

  in.read(reinterpret_cast<char*>(&version), 4);
  in.read(reinterpret_cast<char*>(&fps), 4);
  in.read(reinterpret_cast<char*>(&frameCount), 4);
  in.read(reinterpret_cast<char*>(&audioBytes), 4);
  in.read(reinterpret_cast<char*>(&videoOffset), 4);

  if (version != 1 || frameCount == 0 || fps == 0) {
    gVvidPlaying.store(false);
    return;
  }

  // 2. Read audio WAV bytes and write to temp WAV
  std::string tempWav = "/tmp/vector-vvid.wav";
  if (audioBytes > 0) {
    std::vector<char> audioBuf(audioBytes);
    in.read(audioBuf.data(), audioBytes);

    std::ofstream outWav(tempWav, std::ios::binary);
    if (outWav) {
      outWav.write(audioBuf.data(), audioBytes);
      outWav.close();

      // Start playing WAV in background via aplay
      runCommand("killall aplay >/dev/null 2>&1");
      runCommand("/etc/initscripts/anki-audio-init >/tmp/vector-hw-audio-init.log 2>&1");
      setAudioVolumePercent(gAudioVolumePercent.load());
      runCommand("(aplay " + shellQuote(tempWav) + " >/tmp/vector-vvid-aplay.log 2>&1 &)");
    }
  }

  // 3. Play video frames loop
  in.seekg(videoOffset);

  std::vector<char> frameBuf(kLcdWidth * kLcdHeight * 2); // 35328 bytes
  auto startTime = std::chrono::steady_clock::now();
  const int frameDurationMs = 1000 / fps;

  for (uint32_t i = 0; i < frameCount && gVvidPlaying.load(); ++i) {
    in.read(frameBuf.data(), frameBuf.size());
    if (in.gcount() < static_cast<std::streamsize>(frameBuf.size())) break;

    gLcd.drawFrame(std::string(frameBuf.data(), frameBuf.size()));

    auto targetTime = startTime + std::chrono::milliseconds((i + 1) * frameDurationMs);
    std::this_thread::sleep_until(targetTime);
  }

  // 4. Cleanup
  runCommand("killall aplay >/dev/null 2>&1");
  unlink(tempWav.c_str());
  gVvidPlaying.store(false);
}



void stopMotorsHard(int repeats = 8) {
  for (int i = 0; i < repeats; ++i) {
    gSpine.setMotors(0, 0, 0, 0, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

struct MotorHoldState {
  bool enabled = false;
  int32_t target = 0;
  double maxPower = 0.65;
  int deadband = 6;

  // PID controller state
  double integral = 0.0;
  int32_t lastError = 0;
  double lastPower = 0.0;
};

std::mutex gMotorHoldMutex;
MotorHoldState gMotorHold[4];
std::atomic<bool> gMotorHoldLoopStarted{false};

struct MotorHoldGains {
  double kp;
  double ki;
  double kd;
  double minPower;
  double slewStep;
  double integralLimit;
};

constexpr MotorHoldGains kMotorHoldGains[4] = {
  // Tracks are heavy and easy to overshoot, so hold corrections must be slow.
  {0.0040, 0.0000, 0.0010, 0.06, 0.030, 0.0},
  {0.0040, 0.0000, 0.0010, 0.06, 0.030, 0.0},
  // Lift/head need less power near target than the old fixed 0.18 floor.
  {0.0035, 0.0004, 0.0012, 0.05, 0.025, 60.0},
  {0.0030, 0.0003, 0.0010, 0.04, 0.020, 50.0},
};

void motorHoldLoop() {
  while (gRunning) {
    bool valid = false;
    BodyToHead snap = gSpine.snapshot(&valid);
    double pw[4] = {0, 0, 0, 0};
    bool active = false;

    if (valid) {
      std::lock_guard<std::mutex> lock(gMotorHoldMutex);
      for (int m = 0; m < 4; ++m) {
        auto& hold = gMotorHold[m];
        if (!hold.enabled) continue;
        int32_t error = hold.target - snap.motor[m].position;
        if (std::abs(error) <= hold.deadband) {
          hold.integral = 0.0;
          hold.lastError = error;
          hold.lastPower = 0.0;
          continue;
        }

        const auto& gains = kMotorHoldGains[m];
        if ((error > 0) != (hold.lastError > 0)) {
          hold.integral = 0.0;
        }
        if (gains.integralLimit > 0.0) {
          hold.integral += error;
          hold.integral = std::clamp(hold.integral, -gains.integralLimit, gains.integralLimit);
        } else {
          hold.integral = 0.0;
        }

        double derivative = hold.lastError == 0 ? 0.0 : static_cast<double>(error - hold.lastError);
        hold.lastError = error;

        double p_term = error * gains.kp;
        double i_term = hold.integral * gains.ki;
        double d_term = derivative * gains.kd;

        double p = powerForEncoderError(m, p_term + i_term + d_term);
        p = std::clamp(p, -hold.maxPower, hold.maxPower);

        if (std::abs(error) > hold.deadband * 4 && std::abs(p) > 0.0 && std::abs(p) < gains.minPower) {
          p = p > 0 ? gains.minPower : -gains.minPower;
        }
        p = std::clamp(p, hold.lastPower - gains.slewStep, hold.lastPower + gains.slewStep);
        hold.lastPower = p;
        pw[m] = p;
        active = true;
      }
    }

    if (active) {
      gSpine.setMotors(pw[0], pw[1], pw[2], pw[3], 120);
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

void ensureMotorHoldLoop() {
  bool expected = false;
  if (gMotorHoldLoopStarted.compare_exchange_strong(expected, true)) {
    std::thread(motorHoldLoop).detach();
  }
}

void disableMotorHold(int motor) {
  if (motor < 0 || motor > 3) return;
  std::lock_guard<std::mutex> lock(gMotorHoldMutex);
  gMotorHold[motor].enabled = false;
  gMotorHold[motor].integral = 0.0;
  gMotorHold[motor].lastError = 0;
  gMotorHold[motor].lastPower = 0.0;
}

void disableAllMotorHolds() {
  std::lock_guard<std::mutex> lock(gMotorHoldMutex);
  for (auto& h : gMotorHold) {
    h.enabled = false;
    h.integral = 0.0;
    h.lastError = 0;
    h.lastPower = 0.0;
  }
}

// Runs in a detached thread to drive a motor a fixed number of encoder ticks.
void motorPositionThread(MotorPosCmd cmd) {
  const int m = cmd.motor;
  if (m < 0 || m > 3) return;
  gMotorPosCancel[m].store(false);

  // Sample starting position
  bool valid = false;
  BodyToHead snap0 = gSpine.snapshot(&valid);
  if (!valid) return;
  int32_t startPos = snap0.motor[m].position;
  int32_t targetPos = startPos + cmd.ticks;

  // Build power array — zero for all other motors
  auto sendPower = [&](double p) {
    double pw[4] = {0, 0, 0, 0};
    pw[m] = p;
    // Head/lift are motors 2 and 3, tracks are 0 and 1.
    // setMotors(left, right, lift, head, ttl)
    gSpine.setMotors(pw[0], pw[1], pw[2], pw[3], 300);
  };

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::clamp(cmd.timeoutMs, 250, kMotorPositionTtlMs));
  const int tolerance = std::clamp(cmd.tolerance, 1, 200);
  const double maxPower = std::clamp(std::abs(cmd.power), 0.01, 1.0);
  const double minPower = std::clamp(std::abs(cmd.minPower), 0.01, maxPower);
  const double kp = (m == 0 || m == 1) ? 0.0050 : 0.0040;
  const double slew = (m == 0 || m == 1) ? 0.055 : 0.035;
  double lastPower = 0.0;
  int stable = 0;

  while (!gMotorPosCancel[m].load() && std::chrono::steady_clock::now() < deadline) {
    BodyToHead snap = gSpine.snapshot(&valid);
    if (!valid) break;
    int32_t curPos = snap.motor[m].position;
    int32_t error = targetPos - curPos;
    if (std::abs(error) <= tolerance) {
      sendPower(0.0);
      lastPower = 0.0;
      if (++stable >= 3) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(35));
      continue;
    }

    stable = 0;
    double encoderControl = shapedPowerForError(error, maxPower, minPower, kp);
    double targetPower = powerForEncoderError(m, encoderControl);
    double pwr = slewTowards(lastPower, targetPower, slew);
    lastPower = pwr;
    sendPower(pwr);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  stopMotorsHard();
}

void trackDriveThread(TrackDriveCmd cmd) {
  gTrackDriveCancel.store(false);
  gMotorPosCancel[0].store(true);
  gMotorPosCancel[1].store(true);

  bool valid = false;
  BodyToHead start = gSpine.snapshot(&valid);
  if (!valid) return;

  const int32_t leftStart = start.motor[0].position;
  const int32_t rightStart = start.motor[1].position;
  const int direction = cmd.ticks >= 0 ? 1 : -1;
  const int32_t target = cmd.ticks;
  const double maxPower = std::clamp(std::abs(cmd.power), 0.01, 1.0);
  const double minPower = std::clamp(std::abs(cmd.minPower), 0.01, maxPower);
  const int tolerance = std::clamp(cmd.tolerance, 1, 200);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::clamp(cmd.timeoutMs, 250, kMotorPositionTtlMs));
  double lastLeft = 0.0;
  double lastRight = 0.0;
  int stable = 0;

  while (!gTrackDriveCancel.load() && std::chrono::steady_clock::now() < deadline) {
    BodyToHead snap = gSpine.snapshot(&valid);
    if (!valid) break;

    const int32_t leftForward = snap.motor[0].position - leftStart;
    const int32_t rightForward = -(snap.motor[1].position - rightStart);
    const int32_t leftError = target - leftForward;
    const int32_t rightError = target - rightForward;

    if (std::abs(leftError) <= tolerance && std::abs(rightError) <= tolerance) {
      gSpine.setMotors(0, 0, 0, 0, 120);
      lastLeft = 0.0;
      lastRight = 0.0;
      if (++stable >= 3) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(35));
      continue;
    }

    stable = 0;
    const int32_t avgError = (leftError + rightError) / 2;
    double base = shapedPowerForError(avgError, maxPower, minPower, 0.0045);
    if ((base > 0) != (direction > 0)) {
      base = direction * minPower;
    }

    const int32_t leftProgress = leftForward * direction;
    const int32_t rightProgress = rightForward * direction;
    const double sync = std::clamp((leftProgress - rightProgress) * 0.0035, -0.16, 0.16);
    double leftPower = std::clamp(base - direction * sync, -maxPower, maxPower);
    double rightPower = std::clamp(base + direction * sync, -maxPower, maxPower);

    leftPower = slewTowards(lastLeft, leftPower, 0.055);
    rightPower = slewTowards(lastRight, rightPower, 0.055);
    lastLeft = leftPower;
    lastRight = rightPower;

    gSpine.setMotors(leftPower, rightPower, 0, 0, 160);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  stopMotorsHard();
  gTrackDriveCancel.store(false);
}

std::string bodyJson() {
  bool valid = false;
  BodyToHead b = gSpine.snapshot(&valid);
  const bool powerButton = gSpine.powerButtonPressed();
  const uint64_t powerButtonHoldMs = gSpine.powerButtonHoldMs();
  std::ostringstream out;
  out << "{";
  out << "\"valid\":" << (valid ? "true" : "false");
  out << ",\"framecounter\":" << b.framecounter;
  out << ",\"flags\":" << static_cast<unsigned>(b.flags);
  out << ",\"failure_code\":" << b.failureCode;
  out << ",\"battery\":{";
  out << "\"main_voltage_raw\":" << b.battery.main_voltage;
  out << ",\"charger_raw\":" << b.battery.charger;
  out << ",\"temperature_raw\":" << b.battery.temperature;
  out << ",\"flags\":" << b.battery.flags << "}";
  out << ",\"motors\":[";
  const char* motorNames[] = {"left_track", "right_track", "lift", "head"};
  for (int i = 0; i < 4; ++i) {
    if (i) out << ",";
    out << "{\"id\":" << i
        << ",\"name\":\"" << motorNames[i] << "\""
        << ",\"position\":" << b.motor[i].position
        << ",\"delta\":" << b.motor[i].delta
        << ",\"time\":" << b.motor[i].time << "}";
  }
  out << "],\"cliff\":[";
  for (int i = 0; i < 4; ++i) {
    if (i) out << ",";
    out << b.cliffSense[i];
  }
  out << "],\"proximity\":{";
  out << "\"status\":" << static_cast<unsigned>(b.proximity.rangeStatus);
  out << ",\"range_mm\":" << __builtin_bswap16(b.proximity.rangeMM);
  out << ",\"signal_rate\":" << __builtin_bswap16(b.proximity.signalRate);
  out << ",\"ambient_rate\":" << __builtin_bswap16(b.proximity.ambientRate);
  out << ",\"sample_count\":" << __builtin_bswap16(b.proximity.sampleCount) << "}";
  out << ",\"touch\":[" << b.touchLevel[0] << "," << b.touchLevel[1] << "]";
  out << ",\"touch_hires\":[" << b.touchHires[0] << "," << b.touchHires[1] << "]";
  out << ",\"buttons\":{\"power\":" << (powerButton ? "true" : "false")
      << ",\"power_raw\":" << (powerButton ? 1 : 0)
      << ",\"power_hold_ms\":" << powerButtonHoldMs
      << ",\"back\":" << (powerButton ? "true" : "false")
      << ",\"back_raw\":" << (powerButton ? 1 : 0) << "}";
  if (gImu.initialized()) {
    double ax, ay, az, gx, gy, gz;
    gImu.getRaw(ax, ay, az, gx, gy, gz);
    out << ",\"accel\":{\"x\":" << ax << ",\"y\":" << ay << ",\"z\":" << az << "}";
    out << ",\"gyro\":{\"x\":" << gx << ",\"y\":" << gy << ",\"z\":" << gz << "}";
    out << ",\"imu_temp\":" << gImu.getTemp();
  }
  out << "}";
  return out.str();
}

std::string statusJson() {
  std::ostringstream out;
  out << "{";
  out << "\"api_version\":\"" << kApiVersion << "\"";
  out << ",\"spine\":{\"device\":\"" << kSpineDevice << "\",\"present\":"
      << (exists(kSpineDevice) ? "true" : "false") << ",\"connected\":"
      << (gSpine.connected() ? "true" : "false") << "}";
  out << ",\"display\":{\"spidev\":\"" << kLcdDevice << "\",\"present\":"
      << (exists(kLcdDevice) ? "true" : "false")
      << ",\"initialized\":" << (gLcd.initialized() ? "true" : "false")
      << ",\"panel\":\"" << gLcd.panelName() << "\""
      << ",\"width\":" << kLcdWidth << ",\"height\":" << kLcdHeight
      << ",\"format\":\"rgb565le\""
      << ",\"last_error\":\"" << jsonEscape(gLcd.lastError()) << "\""
      << ",\"backlights\":[";
  const char* lights[] = {"/sys/class/leds/face-backlight/brightness",
                          "/sys/class/leds/face-backlight-left/brightness",
                          "/sys/class/leds/face-backlight-right/brightness"};
  for (int i = 0; i < 3; ++i) {
    if (i) out << ",";
    out << "{\"path\":\"" << lights[i] << "\",\"present\":" << (exists(lights[i]) ? "true" : "false") << "}";
  }
  out << "]}";
  out << ",\"imu\":{\"spidev\":\"" << kImuDevice << "\",\"present\":" << (exists(kImuDevice) ? "true" : "false")
      << ",\"initialized\":" << (gImu.initialized() ? "true" : "false")
      << ",\"whoami\":" << static_cast<unsigned>(gImu.whoami())
      << ",\"last_error\":\"" << jsonEscape(gImu.lastError()) << "\"";
  if (gImu.initialized()) {
    double ax, ay, az, gx, gy, gz;
    gImu.getRaw(ax, ay, az, gx, gy, gz);
    out << ",\"accel\":{\"x\":" << ax << ",\"y\":" << ay << ",\"z\":" << az << "}";
    out << ",\"gyro\":{\"x\":" << gx << ",\"y\":" << gy << ",\"z\":" << gz << "}";
  }
  out << "}";
  // Camera: snapshot file status + daemon PID
  pid_t camPid = gCameraDaemonPid.load();
  if (camPid <= 0) camPid = findProcessByComm("mm-qcamera-daem");
  out << ",\"camera\":{\"snapshot_file\":\"" << kCameraSnapshotPath << "\",\"snapshot_available\":"
      << (exists(kCameraSnapshotPath) ? "true" : "false")
      << ",\"daemon_running\":" << (camPid > 0 ? "true" : "false")
      << ",\"daemon_pid\":" << camPid << "}";
  out << ",\"audio\":{\"available\":" << (exists("/usr/bin/aplay") || exists("/bin/aplay") ? "true" : "false")
      << ",\"player\":\"aplay\",\"upload_path\":\"" << kAudioUploadPath << "\"}";
  out << ",\"body\":" << bodyJson();
  out << "}";
  return out.str();
}

std::vector<uint8_t> parseBackpackRgb(const std::string& body) {
  std::vector<uint8_t> out;
  size_t pos = 0;
  while (out.size() < 12) {
    size_t start = body.find('{', pos);
    if (start == std::string::npos) break;
    size_t end = body.find('}', start);
    if (end == std::string::npos) break;

    std::string one = body.substr(start, end - start + 1);
    out.push_back(static_cast<uint8_t>(std::clamp(numberField(one, "r", 0), 0.0, 255.0)));
    out.push_back(static_cast<uint8_t>(std::clamp(numberField(one, "g", 0), 0.0, 255.0)));
    out.push_back(static_cast<uint8_t>(std::clamp(numberField(one, "b", 0), 0.0, 255.0)));

    pos = end + 1;
  }
  return out;
}

const char* kRunScriptVectorRobotModule = R"PYSDK(
import json
import os
import time
import urllib.error
import urllib.request


class VectorRobotError(RuntimeError):
    pass


class VectorRobot:
    _MOTOR_NAME_TO_ID = {
        "left": 0, "left_track": 0,
        "right": 1, "right_track": 1,
        "lift": 2,
        "head": 3,
    }

    _LED_NAME_TO_ID = {
        "back": 0,
        "middle": 1,
        "front": 2,
        "button": 3,
        "status": 3,
    }

    def __init__(self, host=None, port=8080, timeout=5.0):
        if host is None:
            api = os.environ.get("VECTOR_HW_API", "http://127.0.0.1:8080")
            from urllib.parse import urlparse
            parsed = urlparse(api)
            host = parsed.hostname or "127.0.0.1"
            port = parsed.port or port
        self.host = host
        self.port = port
        self.timeout = timeout

    @property
    def base_url(self):
        return "http://%s:%s" % (self.host, self.port)

    def _resolve_motor(self, motor):
        if isinstance(motor, int):
            if motor not in (0, 1, 2, 3):
                raise ValueError("motor must be 0..3")
            return motor
        key = str(motor).lower().strip()
        if key not in self._MOTOR_NAME_TO_ID:
            raise ValueError("unknown motor %r" % (motor,))
        return self._MOTOR_NAME_TO_ID[key]

    def _resolve_led(self, led):
        if isinstance(led, int):
            if led not in (0, 1, 2, 3):
                raise ValueError("led must be 0..3")
            return led
        key = str(led).lower().strip()
        if key not in self._LED_NAME_TO_ID:
            raise ValueError("unknown led %r" % (led,))
        return self._LED_NAME_TO_ID[key]

    def request(self, method, path, body=None, content_type="application/json"):
        data = None
        headers = {}
        if isinstance(body, dict):
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = content_type
        elif isinstance(body, bytes):
            data = body
            headers["Content-Type"] = content_type
        req = urllib.request.Request(self.base_url + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                payload = resp.read()
                ctype = resp.headers.get("Content-Type", "")
        except urllib.error.HTTPError as exc:
            raise VectorRobotError("HTTP %s: %s" % (exc.code, exc.read().decode("utf-8", "replace")))
        if "application/json" in ctype:
            return json.loads(payload.decode("utf-8"))
        return payload

    def status(self):
        return self.request("GET", "/v1/status")

    def sensors(self):
        return self.request("GET", "/v1/sensors")

    def back_button_pressed(self):
        buttons = self.sensors().get("buttons") or {}
        return bool(buttons.get("power", buttons.get("back")))

    def power_button_pressed(self):
        buttons = self.sensors().get("buttons") or {}
        return bool(buttons.get("power", buttons.get("back")))

    def power_button_hold_ms(self):
        buttons = self.sensors().get("buttons") or {}
        return int(buttons.get("power_hold_ms", 0) or 0)

    def motors_state(self):
        return self.request("GET", "/v1/motors/state")

    def get_state(self):
        motors = self.motors_state()
        sensors = self.sensors()
        return {"status": self.status(), "sensors": sensors, "motors": motors.get("motors") or motors.get("motor") or [], "buttons": sensors.get("buttons") or {}}

    def set_motors(self, left=0, right=0, lift=0, head=0, ttl_ms=250):
        return self.request("POST", "/v1/motors", {"left": left, "right": right, "lift": lift, "head": head, "ttl_ms": ttl_ms})

    def stop_motors(self):
        return self.request("POST", "/v1/motors/stop")

    def set_motors_for(self, left=0, right=0, lift=0, head=0, duration=0.25, ttl_ms=180):
        deadline = time.monotonic() + max(0.0, min(float(duration), 5.0))
        try:
            while time.monotonic() < deadline:
                self.set_motors(left, right, lift, head, ttl_ms)
                time.sleep(0.05)
        finally:
            self.stop_motors()
        return {"ok": True}

    def drive_raw(self, left, right, duration):
        return self.set_motors_for(left=left, right=right, duration=duration)

    def move_motor(self, motor, ticks, power=0.5, min_power=None, tolerance=None, timeout_ms=None):
        body = {"motor": self._resolve_motor(motor), "ticks": int(ticks), "power": float(power)}
        if min_power is not None:
            body["min_power"] = float(min_power)
        if tolerance is not None:
            body["tolerance"] = int(tolerance)
        if timeout_ms is not None:
            body["timeout_ms"] = int(timeout_ms)
        return self.request("POST", "/v1/motors/position", body)

    def drive_straight(self, ticks, power=0.45, timeout_ms=10000, min_power=None, tolerance=None):
        body = {"ticks": int(ticks), "power": float(power), "timeout_ms": int(timeout_ms)}
        if min_power is not None:
            body["min_power"] = float(min_power)
        if tolerance is not None:
            body["tolerance"] = int(tolerance)
        return self.request("POST", "/v1/motors/drive", body)

    def hold_motor(self, motor, enabled=True, target=None, power=0.7, deadband=6):
        body = {"motor": self._resolve_motor(motor), "enabled": 1 if enabled else 0}
        if enabled:
            body.update({"power": power, "deadband": deadband})
            if target is not None:
                body["target"] = target
        return self.request("POST", "/v1/motors/hold", body)

    def set_backpack_leds(self, r, g, b):
        return self.request("POST", "/v1/leds/backpack", {"r": int(r), "g": int(g), "b": int(b)})

    def set_backpack_led(self, led, r, g, b):
        led = self._resolve_led(led)
        return self.request("POST", "/v1/leds/backpack", {"led": led, "r": int(r), "g": int(g), "b": int(b)})
)PYSDK";

void handleApps(int fd, const std::string& method, const std::string& path, const std::string& body) {
  if (method == "POST" && path == "/v1/apps/run-script") {
    if (!exists("/usr/bin/python3")) {
      return sendJsonError(fd, 500, "python3 is not installed on the robot");
    }
    if (!writeWholeFile("/tmp/vector_robot.py", kRunScriptVectorRobotModule)) {
      return sendJsonError(fd, 500, "cannot write VectorRobot SDK module to tmp");
    }
    std::string scriptPath = "/tmp/run-script-" + std::to_string(fd) + ".py";
    if (!writeWholeFile(scriptPath, body)) {
      return sendJsonError(fd, 500, "cannot write script to tmp");
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
      unlink(scriptPath.c_str());
      return sendJsonError(fd, 500, "failed to create pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
      close(pipefd[0]);
      close(pipefd[1]);
      unlink(scriptPath.c_str());
      return sendJsonError(fd, 500, "failed to fork");
    }

    if (pid == 0) {
      // Child process
      close(pipefd[0]); // close read end
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);

      char* argv[] = { (char*)"/usr/bin/python3", (char*)"-u", (char*)scriptPath.c_str(), nullptr };
      execvp(argv[0], argv);
      exit(127);
    }

    // Parent process
    close(pipefd[1]); // close write end

    // Send headers first
    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n";
    hdr << "Date: " << httpDate() << "\r\n";
    hdr << "Server: vector-hw-api/" << kApiVersion << "\r\n";
    hdr << "Connection: close\r\n";
    hdr << "Content-Type: text/plain\r\n";
    hdr << "Transfer-Encoding: chunked\r\n\r\n";
    if (!sendAll(fd, hdr.str())) {
      kill(pid, SIGKILL);
      int status;
      waitpid(pid, &status, 0);
      close(pipefd[0]);
      unlink(scriptPath.c_str());
      return;
    }

    char buf[512];
    bool clientAlive = true;
    while (true) {
      struct pollfd pfds[2];
      pfds[0].fd = pipefd[0];
      pfds[0].events = POLLIN;
      pfds[1].fd = fd;
      pfds[1].events = POLLIN | POLLERR | POLLHUP;

      int ret = poll(pfds, 2, 1000); // 1s timeout
      if (ret < 0) {
        if (errno == EINTR) continue;
        break;
      }

      // Check if client disconnected
      if (pfds[1].revents & (POLLERR | POLLHUP)) {
        clientAlive = false;
        kill(pid, SIGKILL);
        break;
      }

      // If there's data to read on the pipe
      if (pfds[0].revents & POLLIN) {
        ssize_t bytesRead = read(pipefd[0], buf, sizeof(buf));
        if (bytesRead <= 0) {
          break; // Pipe closed, child exited
        }
        if (clientAlive) {
          if (!sendChunk(fd, std::string(buf, bytesRead))) {
            clientAlive = false;
            kill(pid, SIGKILL);
            break;
          }
        }
      }

      // Also check if child process has exited
      int status;
      pid_t reaped = waitpid(pid, &status, WNOHANG);
      if (reaped == pid) {
        // Drain any remaining output in the pipe
        while (true) {
          struct pollfd pfd;
          pfd.fd = pipefd[0];
          pfd.events = POLLIN;
          int r = poll(&pfd, 1, 0); // non-blocking check
          if (r > 0 && (pfd.revents & POLLIN)) {
            ssize_t bytesRead = read(pipefd[0], buf, sizeof(buf));
            if (bytesRead > 0) {
              if (clientAlive) {
                sendChunk(fd, std::string(buf, bytesRead));
              }
            } else {
              break;
            }
          } else {
            break;
          }
        }
        break;
      }
    }
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    sendEndChunks(fd);
    unlink(scriptPath.c_str());
    return;
  }

  if (method == "GET" && path == "/v1/apps") {
    FILE* fp = popen("/usr/bin/vector-appctl list 2>&1", "r");
    if (!fp) return sendJsonError(fd, 500, "vector-appctl failed");
    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), fp)) output += buf;
    int rc = pclose(fp);
    if (rc != 0) return sendJsonError(fd, 500, output);
    return sendResponse(fd, 200, "OK", output.empty() ? "[]" : output);
  }

  if (method == "POST" && path == "/v1/apps/install") {
    std::string tmp = "/data/vector-app-upload.tar.gz";
    int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) return sendJsonError(fd, 500, "cannot create upload file");
    write(out, body.data(), body.size());
    close(out);
    std::string cmd = "/usr/bin/vector-appctl install " + shellQuote(tmp) + " 2>&1";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return sendJsonError(fd, 500, "vector-appctl install failed");
    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), fp)) output += buf;
    int rc = pclose(fp);
    unlink(tmp.c_str());
    if (rc != 0) return sendJsonError(fd, 400, output);
    return sendResponse(fd, 200, "OK", output.empty() ? "{\"ok\":true}" : output);
  }

  const std::string prefix = "/v1/apps/";
  if (path.rfind(prefix, 0) == 0) {
    std::string rest = path.substr(prefix.size());
    size_t slash = rest.find('/');
    std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);
    bool okAction = (method == "POST" && (action == "start" || action == "stop")) ||
                    (method == "DELETE" && action.empty());
    if (!okAction) {
      return sendJsonError(fd, 404, "unsupported app action");
    }
    std::string cmd = "/usr/bin/vector-appctl " + std::string(method == "DELETE" ? "delete " : action + " ") + shellQuote(id) + " 2>&1";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return sendJsonError(fd, 500, "vector-appctl action failed");
    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), fp)) output += buf;
    int rc = pclose(fp);
    if (rc != 0) return sendJsonError(fd, 400, output);
    return sendResponse(fd, 200, "OK", output.empty() ? "{\"ok\":true}" : output);
  }

  sendJsonError(fd, 404, "not found");
}

bool isWebSocketRequest(const std::string& request) {
  size_t headerEnd = request.find("\r\n\r\n");
  std::string headersPart = (headerEnd == std::string::npos) ? request : request.substr(0, headerEnd);
  std::string lowerReq = headersPart;
  std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), ::tolower);
  return lowerReq.find("upgrade: websocket") != std::string::npos;
}

std::string headerValue(const std::string& request, const std::string& name) {
  size_t headerEnd = request.find("\r\n\r\n");
  std::string headersPart = (headerEnd == std::string::npos) ? request : request.substr(0, headerEnd);
  std::string lowerReq = headersPart;
  std::string lowerName = name;
  std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), ::tolower);
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
  size_t p = lowerReq.find(lowerName + ":");
  if (p == std::string::npos) return "";
  p = request.find(':', p);
  if (p == std::string::npos) return "";
  ++p;
  while (p < request.size() && (request[p] == ' ' || request[p] == '\t')) ++p;
  size_t e = request.find("\r\n", p);
  if (e == std::string::npos) e = request.size();
  return request.substr(p, e - p);
}

void websocketEvents(int fd, const std::string& request) {
  std::string key = headerValue(request, "Sec-WebSocket-Key");
  if (key.empty()) return sendJsonError(fd, 400, "missing Sec-WebSocket-Key");
  std::string acceptInput = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const uint8_t*>(acceptInput.data()), acceptInput.size(), digest);
  std::string accept = b64(digest, sizeof(digest));
  std::ostringstream hs;
  hs << "HTTP/1.1 101 Switching Protocols\r\n";
  hs << "Upgrade: websocket\r\nConnection: Upgrade\r\n";
  hs << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
  if (!sendAll(fd, hs.str())) return;
  while (gRunning) {
    std::string payload = "{\"type\":\"body_frame\",\"body\":" + bodyJson() + "}";
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (payload.size() < 126) {
      frame.push_back(static_cast<char>(payload.size()));
    } else {
      frame.push_back(126);
      frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
      frame.push_back(static_cast<char>(payload.size() & 0xff));
    }
    frame += payload;
    if (!sendAll(fd, frame)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

void sseEvents(int fd) {
  std::ostringstream hdr;
  hdr << "HTTP/1.1 200 OK\r\n";
  hdr << "Content-Type: text/event-stream\r\n";
  hdr << "Cache-Control: no-cache\r\n";
  hdr << "Connection: close\r\n\r\n";
  if (!sendAll(fd, hdr.str())) return;
  while (gRunning) {
    std::string payload = "event: body_frame\ndata: " + bodyJson() + "\n\n";
    if (!sendAll(fd, payload)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

std::string getQueryParam(const std::string& query, const std::string& key) {
  size_t pos = query.find(key + "=");
  if (pos == std::string::npos) return "";
  pos += key.size() + 1;
  size_t end = query.find('&', pos);
  if (end == std::string::npos) return query.substr(pos);
  return query.substr(pos, end - pos);
}

void handleClient(int fd) {
  std::string req;
  char buf[4096];
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      close(fd);
      return;
    }
    req.append(buf, buf + n);
  }
  size_t headerEnd = req.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    close(fd);
    return;
  }
  std::istringstream first(req.substr(0, req.find("\r\n")));
  std::string method, path, version;
  first >> method >> path >> version;
  size_t qPos = path.find('?');
  std::string query;
  if (qPos != std::string::npos) {
    query = path.substr(qPos + 1);
    path = path.substr(0, qPos);
  }
  size_t contentLength = 0;
  std::string cl = headerValue(req, "Content-Length");
  if (!cl.empty()) contentLength = static_cast<size_t>(strtoul(cl.c_str(), nullptr, 10));
  std::string body = req.substr(headerEnd + 4);
  while (body.size() < contentLength) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    body.append(buf, buf + n);
  }
  if (body.size() > contentLength) body.resize(contentLength);

  if (path == "/v1/events" && isWebSocketRequest(req)) {
    websocketEvents(fd, req);
    close(fd);
    return;
  }
  if (method == "GET" && path == "/v1/events") {
    sseEvents(fd);
    close(fd);
    return;
  }
  if (method == "GET" && path == "/v1/capabilities") {
    // LLM-readable API manifest
    const char* caps = R"JSON({
  "api_version": "0.2.7",
  "base_url": "http://<robot-ip>:8080",
  "description": "Vector robot hardware API. All endpoints are HTTP. Motor power values are -1.0 to 1.0 (float). Encoder ticks are raw int32 from the Spine MCU body frame.",
  "endpoints": [
    {"method":"GET","path":"/v1/status","desc":"Full status: api_version, spine, display, imu, camera, audio, body (sensors+encoders)."},
    {"method":"GET","path":"/v1/sensors","desc":"Raw body sensor snapshot: framecounter, battery, motors[4].position/delta, cliff[4], proximity.range_mm, touch[2], buttons.power, buttons.power_hold_ms. Compatibility aliases buttons.back/back_raw are also present."},
    {"method":"GET","path":"/v1/motors/state","desc":"Current encoder state for all 4 motors. Returns motors array with id (0=left_track, 1=right_track, 2=lift, 3=head), position (int32 ticks), delta (ticks since last frame), time."},
    {"method":"POST","path":"/v1/motors","desc":"Set raw motor power. Body: {left, right, lift, head: -1.0..1.0, ttl_ms: int}. Motors auto-stop when ttl_ms expires."},
    {"method":"POST","path":"/v1/motors/position","desc":"Profiled move of one motor by relative encoder ticks. Body: {motor:0-3, ticks:int signed, power?:0.01..1.0, min_power?:0.01..power, tolerance?:ticks, timeout_ms?:250..10000}. Returns immediately; robot-side controller tapers near target and stops after stable tolerance."},
    {"method":"POST","path":"/v1/motors/drive","desc":"Profiled synchronized straight track drive. Body: {ticks:int physical forward-positive, power?:0.01..1.0, min_power?:0.01..power, tolerance?:ticks, timeout_ms?:250..10000}. Returns immediately; robot-side controller uses forward-normalized encoders, slew limiting, and cross-track sync."},
    {"method":"POST","path":"/v1/motors/hold","desc":"Enable or disable closed-loop encoder hold. Enable body: {motor:0-3, enabled:1, target?:ticks, power?:0.05..1.0, deadband?:ticks}. Disable body: {motor:0-3, enabled:0}."},
    {"method":"POST","path":"/v1/motors/stop","desc":"Cancel any in-progress position commands and zero all motors. No body needed."},
    {"method":"POST","path":"/v1/leds/backpack","desc":"Set backpack LED color. Body: array of up to 4 objects [{r,g,b}] each 0-255."},
    {"method":"POST","path":"/v1/display/init","desc":"Initialize face LCD (required before first frame). Returns {ok, panel: 'santek'|'midas'}."},
    {"method":"POST","path":"/v1/display/frame","desc":"Upload a 184x96 RGB565 little-endian frame (35328 bytes) to the face display. Content-Type: application/octet-stream."},
    {"method":"POST","path":"/v1/display/brightness","desc":"Set face backlight brightness. Body: {level: 0-255}."},
    {"method":"GET","path":"/v1/camera/snapshot","desc":"Captures and returns the current camera frame as image/bmp. Starts the Qualcomm and Anki camera producers on demand."},
    {"method":"GET","path":"/v1/camera/stream","desc":"Multipart image/bmp live stream. Keep connection open to receive frames. Requires camera daemon."},
    {"method":"POST","path":"/v1/camera/daemon/start","desc":"Start mm-qcamera-daemon and mm-anki-camera capture producer. Returns {ok, pid} or {error}."},
    {"method":"POST","path":"/v1/camera/daemon/stop","desc":"Stop camera daemon."},
    {"method":"GET","path":"/v1/audio/status","desc":"Returns {available, player, volume (0-100)}."},
    {"method":"POST","path":"/v1/audio/play","desc":"Upload and play a WAV/PCM file (max 4 MiB). Content-Type: audio/wav. Uses aplay on robot."},
    {"method":"POST","path":"/v1/audio/stop","desc":"Stop audio playback (killall aplay)."},
    {"method":"POST","path":"/v1/audio/volume","desc":"Set playback volume. Body: {level: 0-100}."},
    {"method":"POST","path":"/v1/audio/stream/start","desc":"Start 4-ch 16kHz microphone UDP stream. Body: {ip: string, port: int}. Sends VAUD binary packets."},
    {"method":"POST","path":"/v1/audio/stream/stop","desc":"Stop microphone UDP stream."},
    {"method":"GET","path":"/v1/audio/stream/status","desc":"Returns {active, ip, port} of current mic stream."},
    {"method":"GET","path":"/v1/events","desc":"Server-Sent Events stream of body_frame telemetry at ~5Hz. Also accepts WebSocket upgrade."},
    {"method":"GET","path":"/v1/apps","desc":"List installed local apps via vector-appctl."},
    {"method":"POST","path":"/v1/apps/install","desc":"Install app from uploaded tar.gz. Body: raw tar.gz bytes."},
    {"method":"POST","path":"/v1/apps/run-script","desc":"Upload and run a Python script on the robot. Body: text/x-python or text/plain source. Streams stdout/stderr as HTTP chunked text. Intended for task-specific local control loops; scripts should call /v1/motors/stop or VectorRobot.stop_motors() in cleanup."},
    {"method":"POST","path":"/v1/apps/{id}/start","desc":"Start installed app by id."},
    {"method":"POST","path":"/v1/apps/{id}/stop","desc":"Stop running app by id."},
    {"method":"DELETE","path":"/v1/apps/{id}","desc":"Uninstall app by id."},
    {"method":"GET","path":"/v1/capabilities","desc":"This document. Returns JSON description of all API endpoints."}
  ],
  "motor_ids": {
    "0": "left_track",
    "1": "right_track",
    "2": "lift",
    "3": "head"
  },
  "notes": [
    "Motors 0+1 (tracks) are mirrored: positive power = forward on both; right-track encoder ticks decrease under positive power.",
    "For physical forward-positive track deltas: left_forward_delta = left_raw_delta, right_forward_delta = -right_raw_delta.",
    "Use /v1/motors/drive or /v1/motors/position for animation-like profiled motion. Raw /v1/motors is for joystick/diagnostic power only.",
    "Motor 2 (lift): positive power = up.",
    "Motor 3 (head): positive power = up/forward tilt.",
    "Encoder ticks accumulate indefinitely; use delta for velocity detection.",
    "Camera snapshot file is /tmp/vector-camera-snapshot.bmp; generated from Anki RGB888 shared-memory frames.",
    "Cliff sensors cliff[0-3] < 90 indicates cliff/air detected.",
    "Touch sensor touch[0] > 610 indicates touch active; touch[1] not populated.",
    "The physical rear power button is exposed as buttons.power/buttons.power_hold_ms from Spine PAYLOAD_BOOT_FRAME/MicroBodyToHead.buttonPressed. buttons.back remains as a compatibility alias.",
    "Proximity range_mm 8190/8191 = out of range sentinel."
  ]
})JSON";
    sendResponse(fd, 200, "OK", caps);
  } else if (method == "GET" && path == "/v1/status") {
    sendResponse(fd, 200, "OK", statusJson());
  } else if (method == "GET" && path == "/v1/sensors") {
    sendResponse(fd, 200, "OK", bodyJson());
  } else if (method == "GET" && path == "/v1/motors/state") {
    bool valid = false;
    BodyToHead b = gSpine.snapshot(&valid);
    std::ostringstream out;
    out << "{\"valid\":" << (valid ? "true" : "false");
    out << ",\"motors\":[";
    const char* motorNames[] = {"left_track", "right_track", "lift", "head"};
    for (int i = 0; i < 4; ++i) {
      if (i) out << ",";
      out << "{\"id\":" << i
          << ",\"name\":\"" << motorNames[i] << "\""
          << ",\"position\":" << b.motor[i].position
          << ",\"delta\":" << b.motor[i].delta
          << ",\"time\":" << b.motor[i].time
          << ",\"moving\":" << (b.motor[i].delta != 0 ? "true" : "false")
          << "}";
    }
    out << "]}";
    sendResponse(fd, 200, "OK", out.str());
  } else if (method == "POST" && path == "/v1/motors") {
    disableAllMotorHolds();
    gTrackDriveCancel.store(true);
    int ttl = static_cast<int>(numberField(body, "ttl_ms", kDefaultTtlMs));
    gSpine.setMotors(numberField(body, "left", 0), numberField(body, "right", 0),
                     numberField(body, "lift", 0), numberField(body, "head", 0), ttl);
    sendResponse(fd, 200, "OK", "{\"ok\":true}");
  } else if (method == "POST" && path == "/v1/motors/position") {
    int motor = static_cast<int>(numberField(body, "motor", -1));
    int ticks  = static_cast<int>(numberField(body, "ticks", 0));
    double pwr = std::clamp(std::abs(numberField(body, "power", 0.5)), 0.01, 1.0);
    double minPower = std::clamp(std::abs(numberField(body, "min_power", (motor == 0 || motor == 1) ? 0.25 : 0.08)), 0.01, pwr);
    int tolerance = std::clamp(static_cast<int>(numberField(body, "tolerance", (motor == 0 || motor == 1) ? 12 : 8)), 1, 200);
    int timeoutMs = std::clamp(static_cast<int>(numberField(body, "timeout_ms", kMotorPositionTtlMs)), 250, kMotorPositionTtlMs);
    if (motor < 0 || motor > 3) {
      sendJsonError(fd, 400, "motor must be 0-3 (0=left_track, 1=right_track, 2=lift, 3=head)");
    } else if (ticks == 0) {
      sendJsonError(fd, 400, "ticks must be non-zero");
    } else {
      disableMotorHold(motor);
      if (motor == 0 || motor == 1) gTrackDriveCancel.store(true);
      // Cancel any existing position command for this motor
      gMotorPosCancel[motor].store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      gMotorPosCancel[motor].store(false);
      MotorPosCmd cmd{motor, ticks, pwr, minPower, tolerance, timeoutMs};
      std::thread(motorPositionThread, cmd).detach();
      std::ostringstream out;
      out << "{\"ok\":true,\"motor\":" << motor
          << ",\"ticks\":" << ticks
          << ",\"power\":" << pwr
          << ",\"min_power\":" << minPower
          << ",\"tolerance\":" << tolerance
          << ",\"timeout_ms\":" << timeoutMs << "}";
      sendResponse(fd, 200, "OK", out.str());
    }
  } else if (method == "POST" && path == "/v1/motors/drive") {
    int32_t ticks = static_cast<int32_t>(numberField(body, "ticks", 0));
    double pwr = std::clamp(std::abs(numberField(body, "power", 0.45)), 0.01, 1.0);
    double minPower = std::clamp(std::abs(numberField(body, "min_power", 0.25)), 0.01, pwr);
    int tolerance = std::clamp(static_cast<int>(numberField(body, "tolerance", 12)), 1, 200);
    int timeoutMs = std::clamp(static_cast<int>(numberField(body, "timeout_ms", kMotorPositionTtlMs)),
                               250, kMotorPositionTtlMs);
    if (ticks == 0) {
      sendJsonError(fd, 400, "ticks must be non-zero");
    } else {
      disableMotorHold(0);
      disableMotorHold(1);
      gMotorPosCancel[0].store(true);
      gMotorPosCancel[1].store(true);
      gTrackDriveCancel.store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      gTrackDriveCancel.store(false);
      TrackDriveCmd cmd{ticks, pwr, minPower, tolerance, timeoutMs};
      std::thread(trackDriveThread, cmd).detach();
      std::ostringstream out;
      out << "{\"ok\":true,\"ticks\":" << ticks
          << ",\"power\":" << pwr
          << ",\"min_power\":" << minPower
          << ",\"tolerance\":" << tolerance
          << ",\"timeout_ms\":" << timeoutMs << "}";
      sendResponse(fd, 200, "OK", out.str());
    }
  } else if (method == "POST" && path == "/v1/motors/hold") {
    int motor = static_cast<int>(numberField(body, "motor", -1));
    bool enabled = numberField(body, "enabled", 1) != 0;
    if (motor < 0 || motor > 3) {
      sendJsonError(fd, 400, "motor must be 0-3 (0=left_track, 1=right_track, 2=lift, 3=head)");
    } else if (!enabled) {
      disableMotorHold(motor);
      gMotorPosCancel[motor].store(true);
      if (motor == 0 || motor == 1) gTrackDriveCancel.store(true);
      stopMotorsHard();
      sendResponse(fd, 200, "OK", "{\"ok\":true,\"enabled\":false}");
    } else {
      bool valid = false;
      BodyToHead snap = gSpine.snapshot(&valid);
      if (!valid) {
        sendJsonError(fd, 503, "no live encoder state for motor hold");
      } else {
        int32_t target = static_cast<int32_t>(numberField(body, "target", snap.motor[motor].position));
        double maxPower = std::clamp(std::abs(numberField(body, "power", 0.25)), 0.05, 1.0);
        int deadband = std::clamp(static_cast<int>(numberField(body, "deadband", 6)), 1, 100);
        gMotorPosCancel[motor].store(true);
        {
          std::lock_guard<std::mutex> lock(gMotorHoldMutex);
          gMotorHold[motor].enabled = true;
          gMotorHold[motor].target = target;
          gMotorHold[motor].maxPower = maxPower;
          gMotorHold[motor].deadband = deadband;
          gMotorHold[motor].integral = 0.0;
          gMotorHold[motor].lastError = 0;
          gMotorHold[motor].lastPower = 0.0;
        }
        ensureMotorHoldLoop();
        std::ostringstream out;
        out << "{\"ok\":true,\"enabled\":true,\"motor\":" << motor
            << ",\"target\":" << target
            << ",\"power\":" << maxPower
            << ",\"deadband\":" << deadband << "}";
        sendResponse(fd, 200, "OK", out.str());
      }
    }
  } else if (method == "POST" && path == "/v1/motors/stop") {
    disableAllMotorHolds();
    for (int i = 0; i < 4; ++i) gMotorPosCancel[i].store(true);
    gTrackDriveCancel.store(true);
    stopMotorsHard();
    sendResponse(fd, 200, "OK", "{\"ok\":true}");
  } else if (method == "POST" && path == "/v1/leds/backpack") {
    int led = static_cast<int>(numberField(body, "led", -1));
    if (led == -1) {
      led = static_cast<int>(numberField(body, "index", -1));
    }
    if (led >= 0 && led < 4) {
      uint8_t r = static_cast<uint8_t>(std::clamp(numberField(body, "r", 0), 0.0, 255.0));
      uint8_t g = static_cast<uint8_t>(std::clamp(numberField(body, "g", 0), 0.0, 255.0));
      uint8_t b = static_cast<uint8_t>(std::clamp(numberField(body, "b", 0), 0.0, 255.0));
      gSpine.setBackpackLed(led, r, g, b);
    } else {
      auto rgb = parseBackpackRgb(body);
      gSpine.setBackpack(rgb);
    }
    sendResponse(fd, 200, "OK", "{\"ok\":true}");
  } else if (method == "POST" && path == "/v1/display/brightness") {
    int level = std::clamp(static_cast<int>(numberField(body, "level", 0)), 0, 255);
    bool ok = gLcd.setBrightness(level);
    sendResponse(fd, ok ? 200 : 503, ok ? "OK" : "Unavailable", ok ? "{\"ok\":true}" : "{\"error\":\"no backlight sysfs nodes\"}");
  } else if (method == "POST" && path == "/v1/display/init") {
    if (gLcd.init()) {
      sendResponse(fd, 200, "OK", "{\"ok\":true,\"panel\":\"" + gLcd.panelName() + "\"}");
    } else {
      sendJsonError(fd, 503, gLcd.lastError().empty() ? "display init failed" : gLcd.lastError());
    }
  } else if (method == "POST" && path == "/v1/display/frame") {
    if (gVvidPlaying.load()) {
      sendResponse(fd, 200, "OK", "{\"ok\":true,\"ignored\":true}");
    } else if (gLcd.drawFrame(body)) {
      sendResponse(fd, 200, "OK", "{\"ok\":true,\"width\":184,\"height\":96,\"format\":\"rgb565le\",\"panel\":\"" + gLcd.panelName() + "\"}");
    } else {
      sendJsonError(fd, 503, gLcd.lastError().empty() ? "display frame failed" : gLcd.lastError());
    }
  } else if (method == "POST" && path == "/v1/display/stream") {
    stopVvidPlaying();
    char frameBuf[kLcdWidth * kLcdHeight * 2]; // 35328 bytes
    while (gRunning) {
      size_t readBytes = 0;
      while (readBytes < sizeof(frameBuf) && gRunning) {
        ssize_t n = recv(fd, frameBuf + readBytes, sizeof(frameBuf) - readBytes, 0);
        if (n <= 0) {
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
          }
          break; // Socket closed
        }
        readBytes += n;
      }
      if (readBytes < sizeof(frameBuf)) break;
      if (!gVvidPlaying.load()) {
        gLcd.drawFrame(std::string(frameBuf, sizeof(frameBuf)));
      }
    }
    sendResponse(fd, 200, "OK", "{\"ok\":true}");
  } else if (method == "GET" && path == "/v1/videos") {
    std::ostringstream out;
    out << "[";
    DIR* dir = opendir("/data/video");
    if (dir) {
      struct dirent* entry;
      bool first = true;
      while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() > 5 && name.substr(name.size() - 5) == ".vvid") {
          if (!first) out << ",";
          first = false;
          out << "\"" << jsonEscape(name) << "\"";
        }
      }
      closedir(dir);
    }
    out << "]";
    sendResponse(fd, 200, "OK", out.str());
  } else if (method == "POST" && path == "/v1/videos/upload") {
    std::string filename = getQueryParam(query, "name");
    if (filename.empty() || filename.find('/') != std::string::npos) {
      sendJsonError(fd, 400, "invalid filename");
    } else {
      mkdir("/data/video", 0755);
      std::string dest = "/data/video/" + filename;
      if (writeWholeFile(dest, body)) {
        sendResponse(fd, 200, "OK", "{\"ok\":true}");
      } else {
        sendJsonError(fd, 500, "failed to write video file");
      }
    }
  } else if (method == "POST" && path == "/v1/videos/play") {
    std::string name = stringField(body, "name", "");
    if (name.empty() || name.find('/') != std::string::npos) {
      sendJsonError(fd, 400, "invalid video name");
    } else {
      std::string filepath = "/data/video/" + name;
      if (!exists(filepath)) {
        sendJsonError(fd, 404, "video not found");
      } else {
        stopVvidPlaying();
        gVvidPlaying.store(true);
        gVvidCurrentName = name;
        gVvidThread = std::thread(vvidPlayThread, filepath);
        gVvidThread.detach();
        sendResponse(fd, 200, "OK", "{\"ok\":true,\"playing\":\"" + name + "\"}");
      }
    }
  } else if (method == "POST" && path == "/v1/videos/stop") {
    stopVvidPlaying();
    sendResponse(fd, 200, "OK", "{\"ok\":true}");
  } else if (method == "DELETE" && path.rfind("/v1/videos/", 0) == 0) {
    std::string name = path.substr(11);
    if (name.empty() || name.find('/') != std::string::npos) {
      sendJsonError(fd, 400, "invalid video name");
    } else {
      std::string filepath = "/data/video/" + name;
      if (unlink(filepath.c_str()) == 0) {
        sendResponse(fd, 200, "OK", "{\"ok\":true}");
      } else {
        sendJsonError(fd, 404, "failed to delete or file not found");
      }
    }

  } else if (method == "GET" && path == "/v1/camera/snapshot") {
    gCameraEnabled.store(true);
    ensureCameraThreadStarted();
    uint64_t startSeq = 0;
    {
      std::lock_guard<std::mutex> lock(gCameraFrameMutex);
      startSeq = gLatestCameraSeq;
    }
    {
      std::unique_lock<std::mutex> lock(gCameraFrameMutex);
      gCameraFrameCond.wait_for(lock, std::chrono::milliseconds(2500), [&] {
        return gLatestCameraSeq > startSeq || (startSeq == 0 && !gLatestDefaultBmpBytes.empty());
      });
    }
    std::string image;
    std::lock_guard<std::mutex> lock(gCameraFrameMutex);
    image = gLatestDefaultBmpBytes;

    if (image.empty()) {
      sendJsonError(fd, 503, "camera snapshot warming up");
    } else {
      sendResponse(fd, 200, "OK", image, "image/bmp");
    }
  } else if (method == "GET" && path == "/v1/camera/stream") {
    gCameraEnabled.store(true);
    ensureCameraThreadStarted();

    {
      std::ostringstream hdr;
      hdr << "HTTP/1.1 200 OK\r\n";
      hdr << "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n";
      hdr << "Cache-Control: no-cache\r\n";
      hdr << "Connection: close\r\n\r\n";
      if (!sendAll(fd, hdr.str())) { close(fd); return; }
    }

    uint64_t lastSentSeq = 0;
    while (gRunning) {
      if (!gCameraEnabled.load()) break;
      std::string image;
      {
        std::unique_lock<std::mutex> lock(gCameraFrameMutex);
        gCameraFrameCond.wait_for(lock, std::chrono::milliseconds(1500), [&] {
          return gLatestCameraSeq > lastSentSeq || !gCameraEnabled.load() || !gRunning;
        });
        if (!gCameraEnabled.load() || !gRunning) break;
        if (gLatestCameraSeq <= lastSentSeq) continue;
      }

      std::lock_guard<std::mutex> lock(gCameraFrameMutex);
      image = gLatestDefaultBmpBytes;
      lastSentSeq = gLatestCameraSeq;

      if (image.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      std::ostringstream part;
      part << "--frame\r\n";
      part << "Content-Type: image/bmp\r\n";
      part << "Content-Length: " << image.size() << "\r\n\r\n";
      part << image << "\r\n";
      if (!sendAll(fd, part.str())) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(kCameraStreamMs));
    }
    close(fd);
    return;
  } else if (method == "POST" && path == "/v1/camera/daemon/start") {
    gCameraEnabled.store(true);
    ensureCameraThreadStarted();
    std::ostringstream out;
    out << "{\"ok\":true}";
    sendResponse(fd, 200, "OK", out.str());
  } else if (method == "POST" && path == "/v1/camera/daemon/stop") {
    gCameraEnabled.store(false);
    stopCameraDaemon();
    sendResponse(fd, 200, "OK", "{\"ok\":true}");
  } else if (method == "GET" && path == "/v1/audio/status") {
    sendResponse(fd, 200, "OK", "{\"available\":" + std::string((exists("/usr/bin/aplay") || exists("/bin/aplay")) ? "true" : "false") +
                 ",\"player\":\"aplay\",\"volume\":" + std::to_string(gAudioVolumePercent.load()) + "}");
  } else if (method == "POST" && path == "/v1/audio/stop") {
    runCommand("killall aplay >/dev/null 2>&1");
    sendResponse(fd, 200, "OK", "{\"ok\":true}");
  } else if (method == "POST" && path == "/v1/audio/volume") {
    int level = std::clamp(static_cast<int>(numberField(body, "level", 100)), 0, 100);
    if (setAudioVolumePercent(level)) {
      sendResponse(fd, 200, "OK", "{\"ok\":true,\"volume\":" + std::to_string(level) + "}");
    } else {
      sendJsonError(fd, 503, "failed to set audio mixer volume");
    }
  } else if (method == "POST" && path == "/v1/audio/stream/start") {
    std::string ip = stringField(body, "ip", "");
    int port = static_cast<int>(numberField(body, "port", 0));
    std::string error;
    if (ip.empty() || port <= 0) {
      sendJsonError(fd, 400, "expected 'ip' and 'port' in request body");
    } else if (startAudioStream(ip, port, error)) {
      sendResponse(fd, 200, "OK", "{\"ok\":true}");
    } else {
      sendJsonError(fd, 500, error);
    }
  } else if (method == "POST" && path == "/v1/audio/stream/stop") {
    stopAudioStream();
    sendResponse(fd, 200, "OK", "{\"ok\":true}");
  } else if (method == "GET" && path == "/v1/audio/stream/status") {
    std::lock_guard<std::mutex> lock(gAudioStreamMutex);
    char ipBuf[64]{};
    if (gAudioStreamSocket >= 0) {
      inet_ntop(AF_INET, &gAudioStreamAddr.sin_addr, ipBuf, sizeof(ipBuf));
    }
    std::ostringstream out;
    out << "{\"active\":" << (gAudioStreamActive.load() ? "true" : "false");
    out << ",\"ip\":\"" << ipBuf << "\"";
    out << ",\"port\":" << ntohs(gAudioStreamAddr.sin_port) << "}";
    sendResponse(fd, 200, "OK", out.str());
  } else if (method == "POST" && path == "/v1/audio/play") {
    if (body.empty() || body.size() > 4 * 1024 * 1024) {
      sendJsonError(fd, 400, "expected WAV/PCM upload up to 4 MiB");
    } else if (!writeWholeFile(kAudioUploadPath, body)) {
      sendJsonError(fd, 500, "cannot store audio upload");
    } else {
      runCommand("killall aplay >/dev/null 2>&1");
      runCommand("/etc/initscripts/anki-audio-init >/tmp/vector-hw-audio-init.log 2>&1");
      setAudioVolumePercent(gAudioVolumePercent.load());
      int rc = runCommand("(aplay " + shellQuote(kAudioUploadPath) + " >/tmp/vector-hw-aplay.log 2>&1 &)");
      sendResponse(fd, rc == 0 ? 200 : 500, rc == 0 ? "OK" : "Error",
                   rc == 0 ? "{\"ok\":true,\"player\":\"aplay\"}" : "{\"error\":\"failed to start aplay\"}");
    }
  } else if (path.rfind("/v1/apps", 0) == 0) {
    handleApps(fd, method, path, body);
  } else {
    sendJsonError(fd, 404, "not found");
  }
  close(fd);
}

void handleSignal(int) {
  gRunning = false;
}

int parsePort(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--listen") {
      std::string listen = argv[i + 1];
      size_t colon = listen.rfind(':');
      if (colon != std::string::npos) {
        return std::atoi(listen.substr(colon + 1).c_str());
      }
      if (!listen.empty() && std::all_of(listen.begin(), listen.end(), ::isdigit)) {
        return std::atoi(listen.c_str());
      }
    }
  }
  return kDefaultPort;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--stop-motors") {
      return sendStopMotorsOnce() ? 0 : 1;
    }
  }

  signal(SIGINT, handleSignal);
  signal(SIGTERM, handleSignal);
  signal(SIGPIPE, SIG_IGN);

  joinSupplementaryGroupIfPresent("camera");

  gSpine.start();
  gImu.init();

  int port = parsePort(argc, argv);
  int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (srv < 0) {
    perror("socket");
    return 1;
  }
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    close(srv);
    return 1;
  }
  if (listen(srv, 16) < 0) {
    perror("listen");
    close(srv);
    return 1;
  }

  ensureCameraThreadStarted();

  while (gRunning) {
    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    int fd = accept4(srv, reinterpret_cast<sockaddr*>(&peer), &peerLen, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    std::thread(handleClient, fd).detach();
  }

  close(srv);
  gImu.stop();
  gSpine.stop();
  if (gCameraThread.joinable()) gCameraThread.join();
  return 0;
}
