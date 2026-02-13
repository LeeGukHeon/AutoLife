#include "common/Config.h"
#include "network/UpbitHttpClient.h"

#include <iostream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: AutoLifeCancelOrder <order_uuid>\n";
        return 1;
    }

    const std::string order_uuid = argv[1];

    try {
        auto& cfg = autolife::Config::getInstance();
        cfg.load("config/config.json");

        const std::string access = cfg.getAccessKey();
        const std::string secret = cfg.getSecretKey();
        if (access.empty() || secret.empty()) {
            std::cerr << "Missing API key/secret\n";
            return 1;
        }

        auto client = std::make_shared<autolife::network::UpbitHttpClient>(access, secret);
        auto status = client->getOrder(order_uuid);

        std::cout << "Current state: " << status.value("state", "unknown") << "\n";
        if (status.value("state", "") == "done" || status.value("state", "") == "cancel") {
            std::cout << "Order already terminal; no cancel needed\n";
            return 0;
        }

        auto cancel = client->cancelOrder(order_uuid);
        std::cout << "Cancel response uuid: " << cancel.value("uuid", "") << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Cancel failed: " << e.what() << "\n";
        return 1;
    }
}
