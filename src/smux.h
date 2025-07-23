#ifndef SMUX_H
#define SMUX_H

#include <cstdint>
#include <memory>
#include <vector>
#include <functional>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <system_error>
#include <chrono>

namespace smux {

// 帧类型定义
enum class FrameType : uint8_t {
    SYN = 0x01,   // 建立新流
    FIN = 0x02,   // 关闭流
    RST = 0x03,   // 重置流
    DATA = 0x04,  // 数据帧
    WINDOW_UPDATE = 0x05, // 窗口更新
    PING = 0x06,  // Ping
    PONG = 0x07   // Pong
};

// 帧头部结构
struct FrameHeader {
    uint32_t stream_id;  // 流ID
    FrameType type;      // 帧类型
    uint32_t length;     // 数据长度
    uint32_t window;     // 窗口大小 (仅用于WINDOW_UPDATE)
    uint32_t flags;      // 标志位
};

// 流状态
enum class StreamState : uint8_t {
    INIT = 0,
    SYN_SENT,
    SYN_RECEIVED,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    CLOSE_WAIT,
    CLOSING,
    LAST_ACK,
    TIME_WAIT,
    CLOSED
};

// 配置选项
struct Config {
    size_t max_streams = 1024;       // 最大流数量
    size_t initial_window_size = 65535; // 初始窗口大小
    size_t max_frame_size = 4096;    // 最大帧大小
    std::chrono::milliseconds keep_alive_interval = std::chrono::milliseconds(30000); // 保活间隔
    std::chrono::milliseconds keep_alive_timeout = std::chrono::milliseconds(10000);  // 保活超时
};

class Smux; // 前向声明

class Stream; // 前向声明


// 多路复用器类
class Smux {
public:
    Smux(const Config& config);
    ~Smux();

    // 禁止拷贝和移动
    Smux(const Smux&) = delete;
    Smux& operator=(const Smux&) = delete;
    Smux(Smux&&) = delete;
    Smux& operator=(Smux&&) = delete;

    // 设置底层IO读写函数
    using ReadFunc = std::function<ssize_t(uint8_t*, size_t)>;
    using WriteFunc = std::function<ssize_t(const uint8_t*, size_t)>;
    void set_io_functions(ReadFunc read_func, WriteFunc write_func);

    // 创建新流
    std::shared_ptr<Stream> open_stream();

    // 接受新流
    std::shared_ptr<Stream> accept_stream();

    // 关闭多路复用器
    void close();

    // 运行事件循环
    void run();

    // 停止事件循环
    void stop();

    // 检查是否正在运行
    bool is_running() const;

private:
    friend class Stream;

    class Impl;
    std::shared_ptr<Impl> impl_;
};

// 流类
class Stream {
public:
    // 读取数据
    ssize_t read(uint8_t* buffer, size_t size);
    ssize_t read(uint8_t* buffer, size_t size, std::chrono::milliseconds timeout);

    // 写入数据
    ssize_t write(const uint8_t* buffer, size_t size);
    ssize_t write(const uint8_t* buffer, size_t size, std::chrono::milliseconds timeout);

    // 关闭流
    void close();

    // 获取流ID
    uint32_t id() const;

    // 检查流是否可写
    bool is_writable() const;

    // 检查流是否可读
    bool is_readable() const;

    // 检查流是否已关闭
    bool is_closed() const;

private:
    friend class Smux;
    friend class Smux::Impl;

    // 构造函数只能由Smux或Smux::Impl创建
    Stream(std::weak_ptr<Smux::Impl> smux_impl, uint32_t stream_id);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 异常类
class SmuxError : public std::system_error {
public:
    enum ErrorCode {
        INVALID_FRAME = 1,
        STREAM_CLOSED,
        STREAM_RESET,
        BUFFER_FULL,
        TIMEOUT,
        IO_ERROR,
        NOT_RUNNING
    };

    SmuxError(ErrorCode code, const std::string& what_arg);
    SmuxError(ErrorCode code, const char* what_arg);

    ErrorCode code() const;
};

} // namespace smux

#endif // SMUX_H
