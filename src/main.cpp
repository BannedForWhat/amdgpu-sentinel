#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <array>
#include <charconv>
#include <dirent.h>
#include <cstdio>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <csignal>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>

enum class MetricType : int {
    VRAM_TOTAL = 0,
    VRAM_USED  = 1,
    GTT_TOTAL  = 2,
    GTT_USED   = 3,
    GPU_BUSY   = 4,
    TEMP       = 5
};

class MetricReader {
public:
    MetricReader() noexcept {
        file_descriptors_.fill(-1);

        char base_path[128]{};
        if (!find_amdgpu_device_path(base_path, sizeof(base_path))) {
            return;
        }

        char path_buf[256];
        auto open_node = [&](const char* node) noexcept -> int {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", base_path, node);
            return open(path_buf, O_RDONLY | O_CLOEXEC);
        };

        file_descriptors_[0] = open_node("mem_info_vram_total");
        file_descriptors_[1] = open_node("mem_info_vram_used");
        file_descriptors_[2] = open_node("mem_info_gtt_total");
        file_descriptors_[3] = open_node("mem_info_gtt_used");
        file_descriptors_[4] = open_node("gpu_busy_percent");
        file_descriptors_[5] = open_hwmon_temp_fd(base_path);
    }

    ~MetricReader() noexcept {
        for (int fd : file_descriptors_) {
            if (fd >= 0) {
                close(fd);
            }
        }
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return (file_descriptors_[0] >= 0 && file_descriptors_[1] >= 0);
    }

    bool read_metric(MetricType type, uint64_t &last_val, bool &degraded) noexcept {
        const int idx = static_cast<int>(type);
        const int fd = file_descriptors_[idx];
        if (fd < 0) {
            degraded = true;
            return true;
        }

        std::array<char, 64> buf;
        const ssize_t bytes_read = pread(fd, buf.data(), buf.size() - 1, 0);

        if (bytes_read < 0) {
            // Защита от SMU/DPM переходов (GFXOFF, BACO, D3cold)
            if (errno == EAGAIN || errno == EBUSY || errno == EINVAL) {
                degraded = true;
                return true;
            }
            return false;
        }

        if (bytes_read == 0) {
            degraded = true;
            return true;
        }

        const char* start = buf.data();
        const char* end = buf.data() + bytes_read;

        while (start < end && (*start == ' ' || *start == '\t' || *start == '\n')) {
            ++start;
        }

        uint64_t parsed_val = 0;
        const auto [ptr, ec] = std::from_chars(start, end, parsed_val);
        if (ec == std::errc{}) {
            last_val = parsed_val;
            return true;
        }

        degraded = true;
        return true;
    }

private:
    static bool find_amdgpu_device_path(char* out_path, size_t max_len) noexcept {
        for (int i = 0; i < 8; ++i) {
            char test_path[128];
            snprintf(test_path, sizeof(test_path), "/sys/class/drm/card%d/device/mem_info_vram_total", i);
            if (access(test_path, R_OK) == 0) {
                snprintf(out_path, max_len, "/sys/class/drm/card%d/device", i);
                return true;
            }
        }
        return false;
    }

    static int open_hwmon_temp_fd(const char* base_path) noexcept {
        char hwmon_base[256];
        snprintf(hwmon_base, sizeof(hwmon_base), "%s/hwmon", base_path);
        DIR* dir = opendir(hwmon_base);
        if (!dir) {
            return -1;
        }

        struct dirent* entry = nullptr;
        int target_fd = -1;

        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "hwmon", 5) == 0) {
                char temp_path[256];
                int written = snprintf(temp_path, sizeof(temp_path), "%s/%s/temp1_input", hwmon_base, entry->d_name);
                if (written > 0 && static_cast<size_t>(written) < sizeof(temp_path)) {
                    target_fd = open(temp_path, O_RDONLY | O_CLOEXEC);
                    if (target_fd >= 0) {
                        break;
                    }
                }
            }
        }

        closedir(dir);
        return target_fd;
    }

    std::array<int, 6> file_descriptors_{};
};

class SentinelEventLoop {
public:
    static constexpr int MaxEvents = 4;

    bool initialize(uint64_t interval_ns) noexcept {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) return false;

        // Блокируем сигналы в процессе, чтобы они доставлялись строго в signalfd
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) return false;

        signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (signal_fd_ < 0) return false;

        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer_fd_ < 0) return false;

        itimerspec its{};
        its.it_interval.tv_sec = static_cast<time_t>(interval_ns / 1'000'000'000ULL);
        its.it_interval.tv_nsec = static_cast<long>(interval_ns % 1'000'000'000ULL);
        its.it_value = its.it_interval;

        if (timerfd_settime(timer_fd_, 0, &its, nullptr) < 0) return false;

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = timer_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev) < 0) return false;

        ev.data.fd = signal_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, signal_fd_, &ev) < 0) return false;

        return true;
    }

    template <typename Callback>
    void run(Callback&& on_tick) noexcept {
        std::array<epoll_event, MaxEvents> events{};
        bool running = true;

        while (running) {
            const int ready = epoll_pwait(epoll_fd_, events.data(), MaxEvents, -1, nullptr);
            for (int i = 0; i < ready; ++i) {
                const int current_fd = events[static_cast<size_t>(i)].data.fd;
                if (current_fd == timer_fd_) {
                    uint64_t expirations = 0;
                    [[maybe_unused]] const auto r = read(timer_fd_, &expirations, sizeof(expirations));
                    on_tick();
                } else if (current_fd == signal_fd_) {
                    signalfd_siginfo info{};
                    [[maybe_unused]] const auto r = read(signal_fd_, &info, sizeof(info));
                    running = false;
                }
            }
        }
    }

    ~SentinelEventLoop() noexcept {
        if (timer_fd_ >= 0) close(timer_fd_);
        if (signal_fd_ >= 0) close(signal_fd_);
        if (epoll_fd_ >= 0) close(epoll_fd_);
    }

private:
    int epoll_fd_{-1};
    int timer_fd_{-1};
    int signal_fd_{-1};
};

// Безаллокационное форматирование временной метки UTC ISO-8601
static size_t format_timestamp(char* buf, size_t size) noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf{};
    gmtime_r(&ts.tv_sec, &tm_buf);

    return snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                    tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                    tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                    ts.tv_nsec / 1'000'000L);
}

int main() noexcept {
    MetricReader reader;
    if (!reader.is_valid()) {
        const char err[] = "[FATAL] AMDGPU DRM device node not found or permission denied.\n";
        ::write(STDERR_FILENO, err, sizeof(err) - 1);
        return 1;
    }

    SentinelEventLoop loop;
    // Интервал: 1.0 секунда (1'000'000'000 нс)
    if (!loop.initialize(1'000'000'000ULL)) {
        const char err[] = "[FATAL] Failed to initialize Sentinel Event Loop.\n";
        ::write(STDERR_FILENO, err, sizeof(err) - 1);
        return 1;
    }

    const char start_msg[] = "[INFO] amdgpu-sentinel loop active (1.0s interval). Press Ctrl+C to terminate.\n";
    ::write(STDOUT_FILENO, start_msg, sizeof(start_msg) - 1);

    std::array<uint64_t, 6> metrics{};
    bool degraded = false;
    alignas(64) std::array<char, 512> log_buf{};
    std::array<char, 32> time_buf{};

    loop.run([&]() noexcept {
        for (size_t i = 0; i < metrics.size(); ++i) {
            auto type = static_cast<MetricType>(i);
            reader.read_metric(type, metrics[i], degraded);
        }

        const double vram_used_gib = static_cast<double>(metrics[1]) / (1024.0 * 1024.0 * 1024.0);
        const double vram_total_gib = static_cast<double>(metrics[0]) / (1024.0 * 1024.0 * 1024.0);
        const double gtt_used_gib   = static_cast<double>(metrics[3]) / (1024.0 * 1024.0 * 1024.0);
        const double gtt_total_gib  = static_cast<double>(metrics[2]) / (1024.0 * 1024.0 * 1024.0);
        const double vram_pct       = (vram_total_gib > 0.0) ? (vram_used_gib * 100.0 / vram_total_gib) : 0.0;
        const double temp_c         = static_cast<double>(metrics[5]) / 1000.0;

        // Математика TTM Eviction Storm: заполнение VRAM > 85% и вытеснение в GTT > 500 MiB
        const bool ttm_spillover = (vram_pct > 85.0) && (gtt_used_gib > 0.5);

        format_timestamp(time_buf.data(), time_buf.size());

        const int len = snprintf(log_buf.data(), log_buf.size(),
            "[%s] VRAM: %5.2f/%5.2f GiB (%5.1f%%) | GTT: %5.2f/%5.2f GiB | GPU: %3lu%% | TEMP: %4.1f C | TTM: %s%s\n",
            time_buf.data(),
            vram_used_gib, vram_total_gib, vram_pct,
            gtt_used_gib, gtt_total_gib,
            metrics[4], temp_c,
            ttm_spillover ? "SPILLOVER ALERT (12.2x PENALTY)" : "OPTIMAL",
            degraded ? " [SMU_DEGRADED]" : ""
        );

        if (len > 0) {
            ::write(STDOUT_FILENO, log_buf.data(), static_cast<size_t>(len));
        }
    });

    const char stop_msg[] = "\n[INFO] Signal caught via signalfd. Graceful shutdown completed.\n";
    ::write(STDOUT_FILENO, stop_msg, sizeof(stop_msg) - 1);

    return 0;
}
