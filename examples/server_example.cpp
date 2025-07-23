#include "../src/smux.h"
#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>

int main() {
    // 创建socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "创建socket失败" << std::endl;
        return 1;
    }

    // 设置socket选项，允许端口重用
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        std::cerr << "设置socket选项失败" << std::endl;
        return 1;
    }

    // 配置服务器地址
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);

    // 绑定socket到端口
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "绑定端口失败" << std::endl;
        return 1;
    }

    // 监听连接
    if (listen(server_fd, 3) < 0) {
        std::cerr << "监听连接失败" << std::endl;
        return 1;
    }

    std::cout << "服务器正在监听端口 8080..." << std::endl;

    // 接受客户端连接
    int new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
    if (new_socket < 0) {
        std::cerr << "接受连接失败" << std::endl;
        return 1;
    }

    std::cout << "客户端已连接: " << inet_ntoa(address.sin_addr) << std::endl;

    // 配置Smux服务器
    smux::Config config = {
        .max_streams = 10,
        .initial_window_size = 65535,
        .max_frame_size = 4096,
        .keep_alive_interval = std::chrono::milliseconds(30000),
        .keep_alive_timeout = std::chrono::milliseconds(10000)
    };

    smux::Smux server(config);

    // 设置IO函数，从socket读写数据
    server.set_io_functions(
        [new_socket](uint8_t* data, size_t size) { return read(new_socket, data, size); },
        [new_socket](const uint8_t* data, size_t size) { return write(new_socket, data, size); }
    );

    // 启动Smux事件循环线程
    std::thread server_thread([&server]() { server.run(); });

    // 等待服务器初始化
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 接受流并读取数据
    auto stream = server.accept_stream();
    if (!stream) {
        std::cerr << "接受流失败" << std::endl;
        return 1;
    }

    uint8_t buffer[1024] = {0};
    ssize_t bytes_read = stream->read(buffer, sizeof(buffer));
    if (bytes_read > 0) {
        std::cout << "收到数据: " << std::string(reinterpret_cast<const char*>(buffer), bytes_read) << std::endl;
    }


    // 等待事件循环结束
    server.stop();
    server_thread.join();

    // 清理资源
    close(new_socket);
    close(server_fd);
    return 0;
}
