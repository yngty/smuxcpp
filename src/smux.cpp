#include "smux.h"
#include <cstring>
#include <algorithm>
#include <thread>
#include <sstream>
#include <iostream>

namespace smux {

// 帧头部大小（字节）
constexpr size_t FRAME_HEADER_SIZE = sizeof(FrameHeader);

// Smux::Impl 实现
class Smux::Impl : public std::enable_shared_from_this<Impl> {
public:
    Impl(const Config& config)
        : config_(config),
          next_stream_id_(1),
          running_(false),
          read_func_(nullptr),
          write_func_(nullptr),
          event_loop_thread_() {
        // 初始化互斥锁和条件变量
        streams_mutex_ = std::make_unique<std::mutex>();
        accept_mutex_ = std::make_unique<std::mutex>();
        accept_cv_ = std::make_unique<std::condition_variable>();
    }

    ~Impl() {
        stop();
        if (event_loop_thread_.joinable()) {
            event_loop_thread_.join();
        }
    }

    void set_io_functions(ReadFunc read_func, WriteFunc write_func) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        read_func_ = std::move(read_func);
        write_func_ = std::move(write_func);
    }

    std::shared_ptr<Stream> open_stream() {
        std::lock_guard<std::mutex> lock(*streams_mutex_);

        if (streams_.size() >= config_.max_streams) {
            throw SmuxError(SmuxError::STREAM_CLOSED, "Max streams reached");
        }

        uint32_t stream_id = next_stream_id_++;
        auto stream = std::shared_ptr<Stream>(new Stream(weak_from_this(), stream_id));
        streams_[stream_id] = StreamInfo(
            stream,
            StreamState::SYN_SENT,
            config_.initial_window_size,
            config_.initial_window_size
        );

        // 发送SYN帧
        send_frame(FrameHeader{
            stream_id,
            FrameType::SYN,
            0,
            static_cast<uint32_t>(config_.initial_window_size),
            0
        }, nullptr);

        return stream;
    }

    std::shared_ptr<Stream> accept_stream() {
        std::unique_lock<std::mutex> lock(*accept_mutex_);
        accept_cv_->wait(lock, [this] { return !pending_streams_.empty() || !running_; });

        if (!running_) {
            return nullptr;
        }

        auto stream = pending_streams_.front();
        pending_streams_.pop_front();
        return stream;
    }

    void close() {
        stop();

        std::lock_guard<std::mutex> lock(*streams_mutex_);
        for (auto& [id, info] : streams_) {
            if (info.state != StreamState::CLOSED) {
                send_frame(FrameHeader{
                    id,
                    FrameType::FIN,
                    0,
                    0,
                    0
                }, nullptr);
                info.state = StreamState::CLOSED;
            }
        }
        streams_.clear();
    }

    void run() {
        if (running_) {
            return;
        }

        running_ = true;
        event_loop_thread_ = std::thread(&Impl::event_loop, this);
    }

    void stop() {
        running_ = false;
        if (event_loop_thread_.joinable()) {
            event_loop_thread_.join();
        }
    }

    bool is_running() const {
        return running_;
    }

    // 从流读取数据
    ssize_t stream_read(uint32_t stream_id, uint8_t* buffer, size_t size, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(*streams_mutex_);
        auto it = streams_.find(stream_id);

        if (it == streams_.end() || it->second.state == StreamState::CLOSED) {
            return -1;
        }

        auto& stream_info = it->second;

        // 如果没有数据且流未关闭，等待数据
        if (stream_info.read_buffer.empty() && stream_info.state != StreamState::CLOSED) {
            if (timeout.count() == 0) {
                return 0;
            }

            auto predicate = [&stream_info]() {
                return !stream_info.read_buffer.empty() || stream_info.state == StreamState::CLOSED;
            };

            if (stream_info.cv->wait_for(lock, timeout, predicate)) {
                if (stream_info.state == StreamState::CLOSED) {
                    return -1;
                }
            } else {
                return 0; // 超时
            }
        }

        if (stream_info.read_buffer.empty()) {
            return 0;
        }

        size_t bytes_to_read = std::min(size, stream_info.read_buffer.size());
        std::memcpy(buffer, stream_info.read_buffer.data(), bytes_to_read);
        stream_info.read_buffer.erase(stream_info.read_buffer.begin(), stream_info.read_buffer.begin() + bytes_to_read);

        // 发送窗口更新
        send_window_update(stream_id, stream_info.recv_window + bytes_to_read);
        stream_info.recv_window += bytes_to_read;

        return bytes_to_read;
    }

    // 向流写入数据
    ssize_t stream_write(uint32_t stream_id, const uint8_t* buffer, size_t size, std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lock(*streams_mutex_);
        auto it = streams_.find(stream_id);

        if (it == streams_.end() || it->second.state == StreamState::CLOSED) {
            return -1;
        }

        auto& stream_info = it->second;

        if (stream_info.send_window == 0) {
            return 0; // 窗口已满
        }

        size_t bytes_to_write = std::min(size, std::min(config_.max_frame_size, stream_info.send_window));

        // 发送数据帧
        send_frame(FrameHeader{
            stream_id,
            FrameType::DATA,
            static_cast<uint32_t>(bytes_to_write),
            0,
            0
        }, buffer);

        stream_info.send_window -= bytes_to_write;

        return bytes_to_write;
    }

    // 关闭流
    void stream_close(uint32_t stream_id) {
        std::lock_guard<std::mutex> lock(*streams_mutex_);
        auto it = streams_.find(stream_id);

        if (it != streams_.end() && it->second.state != StreamState::CLOSED) {
            send_frame(FrameHeader{
                stream_id,
                FrameType::FIN,
                0,
                0,
                0
            }, nullptr);
            it->second.state = StreamState::CLOSED;
            it->second.cv->notify_all();
        }
    }

private:
    friend class Stream;

    struct StreamInfo {
        std::shared_ptr<Stream> stream;
        StreamState state;
        size_t send_window;
        size_t recv_window;
        std::vector<uint8_t> read_buffer;
        std::unique_ptr<std::condition_variable> cv;

        StreamInfo() : cv(std::make_unique<std::condition_variable>()) {}
        StreamInfo(std::shared_ptr<Stream> s, StreamState st, size_t sw, size_t rw)
            : stream(std::move(s)), state(st), send_window(sw), recv_window(rw),
              cv(std::make_unique<std::condition_variable>()) {}
    };

    Config config_;
    uint32_t next_stream_id_;
    bool running_;
    ReadFunc read_func_;
    WriteFunc write_func_;
    std::thread event_loop_thread_;
    std::mutex io_mutex_;
    std::unique_ptr<std::mutex> streams_mutex_;
    std::map<uint32_t, StreamInfo> streams_;
    std::unique_ptr<std::mutex> accept_mutex_;
    std::unique_ptr<std::condition_variable> accept_cv_;
    std::deque<std::shared_ptr<Stream>> pending_streams_;

    // 发送窗口更新
    void send_window_update(uint32_t stream_id, size_t window_size) {
        FrameHeader header{
            stream_id,
            FrameType::WINDOW_UPDATE,
            0,
            static_cast<uint32_t>(window_size),
            0
        };
        send_frame(header, nullptr);
    }

    // 发送帧
    void send_frame(const FrameHeader& header, const uint8_t* data) {
        std::lock_guard<std::mutex> lock(io_mutex_);

        if (!write_func_) {
            return;
        }

        // 分配缓冲区
        std::vector<uint8_t> buffer(FRAME_HEADER_SIZE + header.length);

        // 复制头部
        std::memcpy(buffer.data(), &header, FRAME_HEADER_SIZE);

        // 复制数据
        if (header.length > 0 && data != nullptr) {
            std::memcpy(buffer.data() + FRAME_HEADER_SIZE, data, header.length);
        }

        // 写入数据
        write_func_(buffer.data(), buffer.size());
    }

    // 处理接收到的帧
    void process_frame(const FrameHeader& header, const uint8_t* data) {
        std::lock_guard<std::mutex> lock(*streams_mutex_);
        auto it = streams_.find(header.stream_id);

        switch (header.type) {
            case FrameType::SYN:
                handle_syn_frame(header, data);
                break;

            case FrameType::FIN:
                handle_fin_frame(header, data);
                break;

            case FrameType::RST:
                handle_rst_frame(header, data);
                break;

            case FrameType::DATA:
                handle_data_frame(header, data);
                break;

            case FrameType::WINDOW_UPDATE:
                handle_window_update_frame(header, data);
                break;

            case FrameType::PING:
                handle_ping_frame(header, data);
                break;

            case FrameType::PONG:
                handle_pong_frame(header, data);
                break;

            default:
                // 未知帧类型
                break;
        }
    }

    // 处理SYN帧
    void handle_syn_frame(const FrameHeader& header, const uint8_t* data) {
        if (streams_.size() >= config_.max_streams) {
            return; // 达到最大流数量
        }

        auto it = streams_.find(header.stream_id);
        if (it != streams_.end()) {
            return; // 流已存在
        }

        // 创建新流
        auto stream = std::shared_ptr<Stream>(new Stream(weak_from_this(), header.stream_id));
        streams_[header.stream_id] = StreamInfo(
            stream,
            StreamState::ESTABLISHED,
            header.window,
            config_.initial_window_size
        );

        // 添加到待接受流队列
        std::lock_guard<std::mutex> accept_lock(*accept_mutex_);
        pending_streams_.push_back(stream);
        accept_cv_->notify_one();

        // 发送窗口更新
        send_window_update(header.stream_id, config_.initial_window_size);
    }

    // 处理FIN帧
    void handle_fin_frame(const FrameHeader& header, const uint8_t* data) {
        auto it = streams_.find(header.stream_id);
        if (it == streams_.end()) {
            return;
        }

        auto& stream_info = it->second;
        stream_info.state = StreamState::CLOSED;
        stream_info.cv->notify_all();
    }

    // 处理RST帧
    void handle_rst_frame(const FrameHeader& header, const uint8_t* data) {
        auto it = streams_.find(header.stream_id);
        if (it == streams_.end()) {
            return;
        }

        auto& stream_info = it->second;
        stream_info.state = StreamState::CLOSED;
        stream_info.read_buffer.clear();
        stream_info.cv->notify_all();
    }

    // 处理数据帧
    void handle_data_frame(const FrameHeader& header, const uint8_t* data) {
        auto it = streams_.find(header.stream_id);
        if (it == streams_.end()) {
            return;
        }

        auto& stream_info = it->second;

        // 如果接收窗口不足，忽略数据
        if (stream_info.recv_window < header.length) {
            return;
        }

        // 添加到读取缓冲区
        stream_info.read_buffer.insert(stream_info.read_buffer.end(), data, data + header.length);
        stream_info.recv_window -= header.length;
        stream_info.cv->notify_all();
    }

    // 处理窗口更新帧
    void handle_window_update_frame(const FrameHeader& header, const uint8_t* data) {
        auto it = streams_.find(header.stream_id);
        if (it == streams_.end()) {
            return;
        }

        auto& stream_info = it->second;
        stream_info.send_window = header.window;
    }

    // 处理PING帧
    void handle_ping_frame(const FrameHeader& header, const uint8_t* data) {
        // 发送PONG响应
        FrameHeader pong_header{
            header.stream_id,
            FrameType::PONG,
            header.length,
            0,
            0
        };
        send_frame(pong_header, data);
    }

    // 处理PONG帧
    void handle_pong_frame(const FrameHeader& header, const uint8_t* data) {
        // 可以在这里处理PONG响应，例如更新最后活动时间
    }

    // 事件循环
    void event_loop() {
        while (running_) {
            // 读取帧头部
            FrameHeader header;
            std::vector<uint8_t> header_buffer(FRAME_HEADER_SIZE);

            std::unique_lock<std::mutex> io_lock(io_mutex_);
            if (!read_func_) {
                io_lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            ssize_t bytes_read = read_func_(header_buffer.data(), FRAME_HEADER_SIZE);
            if (bytes_read != FRAME_HEADER_SIZE) {
                io_lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // 解析头部
            std::memcpy(&header, header_buffer.data(), FRAME_HEADER_SIZE);

            // 读取数据部分
            std::vector<uint8_t> data;
            if (header.length > 0) {
                data.resize(header.length);
                bytes_read = read_func_(data.data(), header.length);
                if (bytes_read != static_cast<ssize_t>(header.length)) {
                    io_lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }

            io_lock.unlock();

            // 处理帧
            process_frame(header, data.data());
        }
    }
};

// Stream::Impl 实现
class Stream::Impl {
public:
    Impl(std::weak_ptr<Smux::Impl> smux_impl, uint32_t stream_id)
        : smux_impl_(std::move(smux_impl)),
          stream_id_(stream_id) {}

    ssize_t read(uint8_t* buffer, size_t size) {
        return read(buffer, size, std::chrono::milliseconds(0));
    }

    ssize_t read(uint8_t* buffer, size_t size, std::chrono::milliseconds timeout) {
        auto smux_ptr = smux_impl_.lock();
        if (!smux_ptr) {
            return -1;
        }

        return smux_ptr->stream_read(stream_id_, buffer, size, timeout);
    }

    ssize_t write(const uint8_t* buffer, size_t size) {
        return write(buffer, size, std::chrono::milliseconds(0));
    }

    ssize_t write(const uint8_t* buffer, size_t size, std::chrono::milliseconds timeout) {
        auto smux_ptr = smux_impl_.lock();
        if (!smux_ptr) {
            return -1;
        }

        return smux_ptr->stream_write(stream_id_, buffer, size, timeout);
    }

    void close() {
        auto smux_ptr = smux_impl_.lock();
        if (smux_ptr) {
            smux_ptr->stream_close(stream_id_);
        }
    }

    uint32_t id() const {
        return stream_id_;
    }

    bool is_writable() const {
        auto smux_ptr = smux_impl_.lock();
        if (!smux_ptr) {
            return false;
        }

        std::lock_guard<std::mutex> lock(*smux_ptr->streams_mutex_);
        auto it = smux_ptr->streams_.find(stream_id_);
        if (it == smux_ptr->streams_.end()) {
            return false;
        }

        return it->second.state != StreamState::CLOSED && it->second.send_window > 0;
    }

    bool is_readable() const {
        auto smux_ptr = smux_impl_.lock();
        if (!smux_ptr) {
            return false;
        }

        std::lock_guard<std::mutex> lock(*smux_ptr->streams_mutex_);
        auto it = smux_ptr->streams_.find(stream_id_);
        if (it == smux_ptr->streams_.end()) {
            return false;
        }

        return it->second.state != StreamState::CLOSED && !it->second.read_buffer.empty();
    }

    bool is_closed() const {
        auto smux_ptr = smux_impl_.lock();
        if (!smux_ptr) {
            return true;
        }

        std::lock_guard<std::mutex> lock(*smux_ptr->streams_mutex_);
        auto it = smux_ptr->streams_.find(stream_id_);
        if (it == smux_ptr->streams_.end()) {
            return true;
        }

        return it->second.state == StreamState::CLOSED;
    }

private:
    std::weak_ptr<Smux::Impl> smux_impl_;
    uint32_t stream_id_;
};

// Smux 实现
Smux::Smux(const Config& config) : impl_(std::make_shared<Impl>(config)) {}

Smux::~Smux() = default;

void Smux::set_io_functions(ReadFunc read_func, WriteFunc write_func) {
    impl_->set_io_functions(std::move(read_func), std::move(write_func));
}

std::shared_ptr<Stream> Smux::open_stream() {
    return impl_->open_stream();
}

std::shared_ptr<Stream> Smux::accept_stream() {
    return impl_->accept_stream();
}

void Smux::close() {
    impl_->close();
}

void Smux::run() {
    impl_->run();
}

void Smux::stop() {
    impl_->stop();
}

bool Smux::is_running() const {
    return impl_->is_running();
}

// Stream 实现
Stream::Stream(std::weak_ptr<Smux::Impl> smux_impl, uint32_t stream_id)
    : impl_(std::make_unique<Impl>(std::move(smux_impl), stream_id)) {}

ssize_t Stream::read(uint8_t* buffer, size_t size) {
    return impl_->read(buffer, size);
}

ssize_t Stream::read(uint8_t* buffer, size_t size, std::chrono::milliseconds timeout) {
    return impl_->read(buffer, size, timeout);
}

ssize_t Stream::write(const uint8_t* buffer, size_t size) {
    return impl_->write(buffer, size);
}

ssize_t Stream::write(const uint8_t* buffer, size_t size, std::chrono::milliseconds timeout) {
    return impl_->write(buffer, size, timeout);
}

void Stream::close() {
    impl_->close();
}

uint32_t Stream::id() const {
    return impl_->id();
}

bool Stream::is_writable() const {
    return impl_->is_writable();
}

bool Stream::is_readable() const {
    return impl_->is_readable();
}

bool Stream::is_closed() const {
    return impl_->is_closed();
}

// SmuxError 实现
SmuxError::SmuxError(ErrorCode code, const std::string& what_arg)
    : std::system_error(static_cast<int>(code), std::generic_category(), what_arg) {}

SmuxError::SmuxError(ErrorCode code, const char* what_arg)
    : std::system_error(static_cast<int>(code), std::generic_category(), what_arg) {}

SmuxError::ErrorCode SmuxError::code() const {
    return static_cast<ErrorCode>(std::system_error::code().value());
}

} // namespace smux
