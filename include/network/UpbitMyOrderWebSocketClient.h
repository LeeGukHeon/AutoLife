#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace autolife {
namespace network {

class UpbitMyOrderWebSocketClient {
public:
    using MessageHandler = std::function<void(const nlohmann::json&)>;

    UpbitMyOrderWebSocketClient(std::string access_key, std::string secret_key);
    ~UpbitMyOrderWebSocketClient();

    bool start(MessageHandler handler);
    void stop();

    bool isConnected() const { return connected_.load(); }
    long long getLastMessageTimeMs() const { return last_message_time_ms_.load(); }

private:
    void runLoop();
    void connectAndReadLoop();
    void dispatchMessage(const std::string& payload);

    std::string access_key_;
    std::string secret_key_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<long long> last_message_time_ms_{0};
    std::thread worker_thread_;

    mutable std::mutex handler_mutex_;
    MessageHandler message_handler_;
};

} // namespace network
} // namespace autolife
