#include <gtest/gtest.h>
#include "../src/smux.h"
#include <thread>
#include <chrono>
#include <cstring>

namespace smux {
namespace test {

// 测试配置
const Config kTestConfig = {
    .max_streams = 10,
    .initial_window_size = 65535,
    .max_frame_size = 4096,
    .keep_alive_interval = std::chrono::milliseconds(30000),
    .keep_alive_timeout = std::chrono::milliseconds(10000)
};

// 测试缓冲区
class TestBuffer {
public:
    TestBuffer() : read_pos_(0), write_pos_(0) {}

    ssize_t read(uint8_t* buffer, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t bytes_to_read = std::min(size, write_pos_ - read_pos_);
        if (bytes_to_read == 0) return 0;

        std::memcpy(buffer, data_ + read_pos_, bytes_to_read);
        read_pos_ += bytes_to_read;
        return bytes_to_read;
    }

    ssize_t write(const uint8_t* buffer, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t bytes_to_write = std::min(size, sizeof(data_) - write_pos_);
        if (bytes_to_write == 0) return 0;

        std::memcpy(data_ + write_pos_, buffer, bytes_to_write);
        write_pos_ += bytes_to_write;
        return bytes_to_write;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        read_pos_ = 0;
        write_pos_ = 0;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return write_pos_ - read_pos_;
    }

private:
    mutable std::mutex mutex_;
    uint8_t data_[4096 * 10]; // 40KB缓冲区
    size_t read_pos_;
    size_t write_pos_;
};

// 测试Smux基本功能
TEST(SmuxTest, BasicFunctionality) {
    TestBuffer buffer;

    // 创建两个Smux实例，模拟客户端和服务器
    Smux client(kTestConfig);
    Smux server(kTestConfig);

    // 设置IO函数，将两个Smux实例通过缓冲区连接
    client.set_io_functions(
        [&buffer](uint8_t* data, size_t size) { return buffer.read(data, size); },
        [&buffer](const uint8_t* data, size_t size) { return buffer.write(data, size); }
    );

    server.set_io_functions(
        [&buffer](uint8_t* data, size_t size) { return buffer.read(data, size); },
        [&buffer](const uint8_t* data, size_t size) { return buffer.write(data, size); }
    );

    // 启动事件循环
    std::thread client_thread([&client]() { client.run(); });
    std::thread server_thread([&server]() { server.run(); });

    // 等待Smux实例启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 客户端创建流并写入数据
    auto client_stream = client.open_stream();
    ASSERT_TRUE(client_stream != nullptr);
    ASSERT_TRUE(client_stream->is_writable());

    const std::string test_data = "Hello, Smux!";
    ssize_t bytes_written = client_stream->write(
        reinterpret_cast<const uint8_t*>(test_data.data()),
        test_data.size()
    );

    ASSERT_EQ(bytes_written, test_data.size());

    // 服务器接受流并读取数据
    auto server_stream = server.accept_stream();
    ASSERT_TRUE(server_stream != nullptr);
    ASSERT_TRUE(server_stream->is_readable());

    uint8_t read_buffer[1024] = {0};
    ssize_t bytes_read = server_stream->read(read_buffer, sizeof(read_buffer));

    ASSERT_EQ(bytes_read, test_data.size());
    ASSERT_STREQ(reinterpret_cast<const char*>(read_buffer), test_data.c_str());

    // 关闭流
    client_stream->close();
    server_stream->close();

    // 等待流关闭
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 停止事件循环
    client.stop();
    server.stop();

    // 等待线程结束
    client_thread.join();
    server_thread.join();
}

// 测试多流并发
TEST(SmuxTest, MultipleStreams) {
    TestBuffer buffer;
    Smux client(kTestConfig);
    Smux server(kTestConfig);

    client.set_io_functions(
        [&buffer](uint8_t* data, size_t size) { return buffer.read(data, size); },
        [&buffer](const uint8_t* data, size_t size) { return buffer.write(data, size); }
    );

    server.set_io_functions(
        [&buffer](uint8_t* data, size_t size) { return buffer.read(data, size); },
        [&buffer](const uint8_t* data, size_t size) { return buffer.write(data, size); }
    );

    std::thread client_thread([&client]() { client.run(); });
    std::thread server_thread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 创建多个流
    const int kStreamCount = 5;
    std::vector<std::shared_ptr<Stream>> client_streams;

    for (int i = 0; i < kStreamCount; ++i) {
        auto stream = client.open_stream();
        ASSERT_TRUE(stream != nullptr);
        client_streams.push_back(stream);

        // 向每个流写入不同的数据
        std::string data = "Stream " + std::to_string(i) + ": Test data";
        ssize_t bytes_written = stream->write(
            reinterpret_cast<const uint8_t*>(data.data()),
            data.size()
        );
        ASSERT_EQ(bytes_written, data.size());
    }

    // 接受所有流并验证数据
    for (int i = 0; i < kStreamCount; ++i) {
        auto stream = server.accept_stream();
        ASSERT_TRUE(stream != nullptr);

        uint8_t read_buffer[1024] = {0};
        ssize_t bytes_read = stream->read(read_buffer, sizeof(read_buffer));
        ASSERT_GT(bytes_read, 0);

        std::string expected_data = "Stream " + std::to_string(i) + ": Test data";
        ASSERT_EQ(std::string(reinterpret_cast<const char*>(read_buffer), bytes_read), expected_data);

        stream->close();
    }

    // 关闭客户端流
    for (auto& stream : client_streams) {
        stream->close();
    }

    // 停止事件循环
    client.stop();
    server.stop();

    // 等待线程结束
    client_thread.join();
    server_thread.join();
}

// 测试窗口机制
TEST(SmuxTest, WindowMechanism) {
    Config config = kTestConfig;
    config.initial_window_size = 1024; // 减小窗口大小以便测试
    config.max_frame_size = 256;

    TestBuffer buffer;
    Smux client(config);
    Smux server(config);

    client.set_io_functions(
        [&buffer](uint8_t* data, size_t size) { return buffer.read(data, size); },
        [&buffer](const uint8_t* data, size_t size) { return buffer.write(data, size); }
    );

    server.set_io_functions(
        [&buffer](uint8_t* data, size_t size) { return buffer.read(data, size); },
        [&buffer](const uint8_t* data, size_t size) { return buffer.write(data, size); }
    );

    std::thread client_thread([&client]() { client.run(); });
    std::thread server_thread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto client_stream = client.open_stream();
    ASSERT_TRUE(client_stream != nullptr);

    auto server_stream = server.accept_stream();
    ASSERT_TRUE(server_stream != nullptr);

    // 写入超过窗口大小的数据
    const size_t kLargeDataSize = 2048; // 2KB，超过1KB的窗口大小
    std::vector<uint8_t> large_data(kLargeDataSize, 'A');

    // 第一次写入应该只能写入窗口大小的数据
    ssize_t bytes_written = client_stream->write(large_data.data(), large_data.size());
    ASSERT_EQ(bytes_written, config.initial_window_size);

    // 此时应该无法继续写入
    bytes_written = client_stream->write(large_data.data() + config.initial_window_size, large_data.size() - config.initial_window_size);
    ASSERT_EQ(bytes_written, 0);

    // 服务器读取数据，触发窗口更新
    uint8_t read_buffer[1024] = {0};
    ssize_t bytes_read = server_stream->read(read_buffer, sizeof(read_buffer));
    ASSERT_EQ(bytes_read, config.initial_window_size);

    // 等待窗口更新传播
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 现在应该可以继续写入数据
    bytes_written = client_stream->write(large_data.data() + config.initial_window_size, large_data.size() - config.initial_window_size);
    ASSERT_EQ(bytes_written, large_data.size() - config.initial_window_size);

    // 关闭流
    client_stream->close();
    server_stream->close();

    // 停止事件循环
    client.stop();
    server.stop();

    // 等待线程结束
    client_thread.join();
    server_thread.join();
}

} // namespace test
} // namespace smux

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
