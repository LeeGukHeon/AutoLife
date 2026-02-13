#include "network/UpbitMyOrderWebSocketClient.h"

#include "common/Logger.h"
#include "network/JwtGenerator.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/ssl.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}
}

namespace autolife {
namespace network {

UpbitMyOrderWebSocketClient::UpbitMyOrderWebSocketClient(std::string access_key, std::string secret_key)
    : access_key_(std::move(access_key))
    , secret_key_(std::move(secret_key)) {}

UpbitMyOrderWebSocketClient::~UpbitMyOrderWebSocketClient() {
    stop();
}

bool UpbitMyOrderWebSocketClient::start(MessageHandler handler) {
    if (running_.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        message_handler_ = std::move(handler);
    }

    running_ = true;
    worker_thread_ = std::thread(&UpbitMyOrderWebSocketClient::runLoop, this);
    return true;
}

void UpbitMyOrderWebSocketClient::stop() {
    running_ = false;
    connected_ = false;

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void UpbitMyOrderWebSocketClient::runLoop() {
    int reconnect_attempt = 0;

    while (running_.load()) {
        const auto connected_since = std::chrono::steady_clock::now();
        try {
            connectAndReadLoop();
            reconnect_attempt = 0;
        } catch (const std::exception& e) {
            connected_ = false;
            if (!running_.load()) {
                break;
            }

            const auto connected_for = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - connected_since
            ).count();
            if (connected_for >= 60) {
                reconnect_attempt = 0;
            } else {
                ++reconnect_attempt;
            }
            const int backoff_seconds = std::min(30, reconnect_attempt * 2);
            LOG_WARN("myOrder WS disconnected: {} (retry in {}s)", e.what(), backoff_seconds);
            std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
        }
    }
}

void UpbitMyOrderWebSocketClient::connectAndReadLoop() {
    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    namespace net = boost::asio;
    namespace ssl = boost::asio::ssl;
    using tcp = boost::asio::ip::tcp;

    net::io_context ioc;
    ssl::context ssl_ctx(ssl::context::tlsv12_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);

    websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ssl_ctx);
    ws.set_option(websocket::stream_base::timeout{
        std::chrono::seconds(15),   // handshake timeout
        std::chrono::seconds(90),   // idle timeout
        true                        // send ping automatically
    });

    const std::string host = "api.upbit.com";
    const std::string port = "443";
    const std::string target = "/websocket/v1/private";

    tcp::resolver resolver(ioc);
    auto results = resolver.resolve(host, port);
    net::connect(ws.next_layer().next_layer(), results.begin(), results.end());
    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
        throw std::runtime_error("myOrder WS SNI setup failed");
    }
    ws.next_layer().set_verify_callback(ssl::host_name_verification(host));
    ws.next_layer().handshake(ssl::stream_base::client);

    const std::string bearer_token = "Bearer " + JwtGenerator::generate(access_key_, secret_key_, {});
    ws.set_option(websocket::stream_base::decorator(
        [bearer_token](websocket::request_type& req) {
            req.set(boost::beast::http::field::authorization, bearer_token);
            req.set(boost::beast::http::field::user_agent, "AutoLife/1.0");
        }
    ));

    ws.handshake(host, target);

    nlohmann::json subscribe = nlohmann::json::array();
    subscribe.push_back({{"ticket", JwtGenerator::generateUUID()}});
    subscribe.push_back({{"type", "myOrder"}});
    subscribe.push_back({{"format", "DEFAULT"}});
    ws.write(net::buffer(subscribe.dump()));

    connected_ = true;
    last_message_time_ms_ = nowMs();
    LOG_INFO("myOrder WS connected");

    ws.control_callback([this](websocket::frame_type kind, beast::string_view) {
        if (kind == websocket::frame_type::pong || kind == websocket::frame_type::ping) {
            last_message_time_ms_ = nowMs();
        }
    });

    beast::flat_buffer buffer;

    while (running_.load()) {
        boost::system::error_code ec;
        ws.read(buffer, ec);
        if (!ec) {
            const std::string payload = beast::buffers_to_string(buffer.cdata());
            buffer.consume(buffer.size());
            last_message_time_ms_ = nowMs();
            dispatchMessage(payload);
        } else if (ec == boost::asio::error::operation_aborted && !running_.load()) {
            break;
        } else if (ec == beast::error::timeout) {
            throw std::runtime_error("myOrder WS timed out");
        } else if (ec == websocket::error::closed) {
            throw std::runtime_error("myOrder WS closed by server");
        } else {
            throw std::runtime_error("myOrder WS read failed: " + ec.message());
        }
    }

    boost::system::error_code close_ec;
    ws.close(websocket::close_code::normal, close_ec);
    connected_ = false;
    if (close_ec && close_ec != websocket::error::closed) {
        LOG_WARN("myOrder WS close warning: {}", close_ec.message());
    } else {
        LOG_INFO("myOrder WS stopped");
    }
}

void UpbitMyOrderWebSocketClient::dispatchMessage(const std::string& payload) {
    try {
        const auto message = nlohmann::json::parse(payload);

        MessageHandler handler_copy;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler_copy = message_handler_;
        }
        if (!handler_copy) {
            return;
        }

        if (message.is_array()) {
            for (const auto& item : message) {
                if (item.is_object()) {
                    handler_copy(item);
                }
            }
            return;
        }

        if (message.is_object()) {
            handler_copy(message);
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to parse myOrder WS message: {}", e.what());
    }
}

} // namespace network
} // namespace autolife
