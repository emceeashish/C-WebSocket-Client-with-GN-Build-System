 #include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

class WebSocketClient {
public:
    WebSocketClient(net::io_context& ioc, const std::string& host, const std::string& port)
        : resolver_(ioc), ws_(ioc), host_(host), port_(port) {}
  // Connect to the server and start receiving thread
    void connect() {
        auto const results = resolver_.resolve(host_, port_);
        net::connect(ws_.next_layer(), results.begin(), results.end());
        ws_.handshake(host_, "/");
        std::cout << "Connected to " << host_ << " on port " << port_ << "\n";
        receiver_thread_ = std::thread(&WebSocketClient::receiveContinuous, this);
    }

    void sendText(const std::string& message) {
        ws_.text(true); // Ensure text mode
        ws_.write(net::buffer(message));
    }

    void sendBinary(const std::vector<uint8_t>& data) {
        ws_.binary(true); // Enable binary mode
        ws_.write(net::buffer(data));
    }

    std::pair<std::string, bool> getResponse() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !messages_.empty(); });
        auto response = messages_.front();
        messages_.pop();
        return response;
    }

    void close() {
        if (ws_.is_open()) {
            ws_.close(websocket::close_code::normal);
        }
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

private:
    void receiveContinuous() {
        try {
            while (ws_.is_open()) {
                beast::flat_buffer buffer;
                ws_.read(buffer);
                std::lock_guard<std::mutex> lock(mutex_);

                if (ws_.got_text()) {
                    messages_.push({beast::buffers_to_string(buffer.data()), true}); // Text message
                } else {
                    std::vector<uint8_t> binaryData(
                        static_cast<const uint8_t*>(buffer.data().data()),
                        static_cast<const uint8_t*>(buffer.data().data()) + buffer.size()
                    );
                    messages_.push({std::string(binaryData.begin(), binaryData.end()), false}); // Binary message
                }
                
                cv_.notify_one();
            }
        } catch (...) {} // Handle closure gracefully
    }

    tcp::resolver resolver_;
    websocket::stream<tcp::socket> ws_;
    std::string host_, port_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::pair<std::string, bool>> messages_; // {message, isText}
    std::thread receiver_thread_;
};

void chatSession(WebSocketClient& client) {
    // Process initial server message (welcome banner)
    try {
        auto response = client.getResponse();
        std::cout << "Server: " << response.first << "\n";
    } catch (...) {}

    std::string input;
    while (true) {
        std::cout << "Enter 'text' to send a text message, 'binary' to send a binary message, or '/exit' to quit: ";
        std::getline(std::cin, input);

        if (input == "/exit") break;
        if (input.empty()) continue;

        if (input == "text") {
            std::cout << "Enter text message: ";
            std::getline(std::cin, input);
            client.sendText(input);
        } else if (input == "binary") {
            std::cout << "Enter binary data as a string (will be converted to binary): ";
            std::getline(std::cin, input);
            std::vector<uint8_t> binaryData(input.begin(), input.end());
            client.sendBinary(binaryData);
        }

        auto response = client.getResponse();
        if (response.second) {
            std::cout << "Server (Text): " << response.first << "\n";
        } else {
            std::cout << "Server (Binary): " << response.first << " (Received binary message)\n";
        }
    }

    client.close();
}

int main() {
    try {
        net::io_context ioc;
        WebSocketClient client(ioc, "echo.websocket.events", "80");
        client.connect();
        chatSession(client);
    } catch (std::exception& e){
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

// (1) User sends a message → client.send("Hello")
//     ↳ ws_.write() → Sends data over WebSocket

// (2) Server sends a response → receiver_thread_ detects it
//     ↳ ws_.read() → Reads message into buffer
//     ↳ messages_.push(buffers_to_string(buffer.data())) → Stores in queue

// (3) Main thread waits for a message → client.GetResponse()
//     ↳ cv_.wait() → Waits until a message is available
//     ↳ messages_.pop() → Retrieves and prints message

// (4) User types /exit → client.close()
//     ↳ ws_.close() → Closes WebSocket connection  why cv is used
