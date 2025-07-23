#include "../src/smux.h"
#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>

int main() {
    // 创建socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "创建socket失败" << std::endl;
        return 1;
    }

    // 配置服务器地址
    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        std::cerr << "无效的服务器地址" << std::endl;
        return 1;
    }

    // 连接服务器
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "连接服务器失败" << std::endl;
        return 1;
    }

    // 配置Smux客户端
    smux::Config config = {
        .max_streams = 10,
        .initial_window_size = 65535,
        .max_frame_size = 4096,
        .keep_alive_interval = std::chrono::milliseconds(30000),
        .keep_alive_timeout = std::chrono::milliseconds(10000)
    };

    smux::Smux client(config);

    // 设置IO函数，从socket读写数据
    client.set_io_functions(
        [sockfd](uint8_t* data, size_t size) { return read(sockfd, data, size); },
        [sockfd](const uint8_t* data, size_t size) { return write(sockfd, data, size); }
    );

    // 打开流并发送数据
    auto stream = client.open_stream();
    if (!stream) {
        std::cerr << "打开流失败" << std::endl;
        return 1;
    }

    std::string message = "Hello from Smux client!";
    ssize_t bytes_written = stream->write(
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );
    std::cout << bytes_written << std::endl;
    if (bytes_written != message.size()) {
        std::cerr << "发送数据失败" << std::endl;
        return 1;
    }
    std::cout << "已发送: " << message << std::endl;

    // 运行Smux事件循环

    std::thread thd([&]() {
        client.run();
    });
    // 清理资源
    thd.join();
    close(sockfd);

    return 0;
}
