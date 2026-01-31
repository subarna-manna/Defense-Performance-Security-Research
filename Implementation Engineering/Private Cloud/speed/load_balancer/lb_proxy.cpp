

// speed/load_balancer/lb_proxy.cpp
// Defense Private Cloud - Low-Latency HTTPS Reverse Proxy / Load Balancer with TLS/mTLS

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include "lb_core.h"       // LoadBalancer, Backend
#include "config_loader.h" // g_config

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;

constexpr size_t BUFFER_SIZE = 8192;

// Forward declarations
class Session;

// Shared SSL context for server (mTLS capable)
ssl::context CreateServerContext() {
    ssl::context ctx(ssl::context::tlsv13_server);  // Prefer TLSv1.3; fallback to v12 if needed

    // Strong options (disable old protocols)
    ctx.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2 |
        ssl::context::no_sslv3 |
        ssl::context::no_tlsv1 |
        ssl::context::no_tlsv1_1 |
        ssl::context::single_dh_use);

    // Load server certificate chain and private key
    ctx.use_certificate_chain_file(g_config.server_cert_path);
    ctx.use_private_key_file(g_config.server_key_path, ssl::context::pem);

    // Optional DH params (for TLS <1.3 compatibility)
    if (!g_config.dh_params_path.empty()) {
        ctx.use_tmp_dh_file(g_config.dh_params_path);
    }

    // mTLS enforcement
    if (g_config.enforce_mtls) {
        ctx.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);
        ctx.load_verify_file(g_config.ca_cert_path);
        // Optional: custom verify callback for extra checks (e.g., cert subject)
        // ctx.set_verify_callback([](bool preverified, ssl::verify_context& ctx) { ... return preverified; });
    } else {
        ctx.set_verify_mode(ssl::verify_none);
    }

    return ctx;
}

// Session handles one client connection over TLS
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, LoadBalancer& lb, ssl::context& server_ctx)
        : stream_(std::move(socket), server_ctx),
          strand_(asio::make_strand(stream_.get_executor())),
          lb_(lb) {}

    void Start() {
        DoHandshake();
    }

private:
    void DoHandshake() {
        auto self = shared_from_this();
        stream_.async_handshake(ssl::stream_base::server,
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec) {
                    if (!ec) {
                        DoReadRequest();
                    } else {
                        std::cerr << "TLS handshake failed: " << ec.message() << std::endl;
                        // Use secure_logger in production
                    }
                }));
    }

    void DoReadRequest() {
        auto self = shared_from_this();
        stream_.async_read_some(
            asio::buffer(request_buffer_),
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, std::size_t length) {
                    if (!ec) {
                        request_data_.append(request_buffer_.data(), length);
                        if (request_data_.find("\r\n\r\n") != std::string::npos) {
                            HandleRequest();
                        } else {
                            DoReadRequest();  // Continue reading
                        }
                    }
                }));
    }

    void HandleRequest() {
        // Basic request parsing (upgrade to Boost.Beast for production)
        size_t pos = request_data_.find("\r\n");
        if (pos == std::string::npos) return;
        std::string request_line = request_data_.substr(0, pos);

        // Select healthy backend
        Backend* backend = lb_.GetNextHealthyBackend();
        if (!backend || !backend->healthy.load()) {
            SendError(503, "No healthy backends");
            return;
        }

        DoConnectToBackend(backend->addr);
    }

    void DoConnectToBackend(const std::string& backend_addr) {
        auto self = shared_from_this();

        size_t colon = backend_addr.find(':');
        if (colon == std::string::npos) return;
        std::string ip = backend_addr.substr(0, colon);
        std::string port = backend_addr.substr(colon + 1);

        tcp::resolver resolver(stream_.get_executor());
        auto endpoints = resolver.resolve(ip, port);

        asio::async_connect(
            backend_socket_,
            endpoints,
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, const tcp::endpoint&) {
                    if (!ec) {
                        // Add X-Forwarded-For
                        size_t pos = request_data_.find("\r\n\r\n");
                        if (pos != std::string::npos) {
                            std::string forwarded = "X-Forwarded-For: " +
                                stream_.next_layer().remote_endpoint().address().to_string() + "\r\n";
                            request_data_.insert(pos, forwarded);
                        }
                        DoWriteToBackend();
                    } else {
                        SendError(502, "Backend connect failed");
                    }
                }));
    }

    void DoWriteToBackend() {
        auto self = shared_from_this();
        asio::async_write(
            backend_socket_,
            asio::buffer(request_data_),
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, std::size_t) {
                    if (!ec) DoReadFromBackend();
                }));
    }

    void DoReadFromBackend() {
        auto self = shared_from_this();
        backend_socket_.async_read_some(
            asio::buffer(response_buffer_),
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, std::size_t length) {
                    if (!ec) {
                        DoWriteToClient(length);
                    }
                }));
    }

    void DoWriteToClient(std::size_t length) {
        auto self = shared_from_this();
        asio::async_write(
            stream_,
            asio::buffer(response_buffer_, length),
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, std::size_t) {
                    if (!ec) DoReadFromBackend();  // Stream response
                }));
    }

    void SendError(int code, const std::string& msg) {
        std::string response = "HTTP/1.1 " + std::to_string(code) + " " + msg + "\r\nContent-Length: " +
                               std::to_string(msg.size()) + "\r\nConnection: close\r\n\r\n" + msg;
        asio::write(stream_, asio::buffer(response));
        stream_.async_shutdown([](auto){});  // Graceful shutdown
    }

    ssl::stream<tcp::socket> stream_;
    tcp::socket backend_socket_{stream_.get_executor()};
    asio::strand<asio::io_context::executor_type> strand_;
    LoadBalancer& lb_;
    char request_buffer_[BUFFER_SIZE]{};
    char response_buffer_[BUFFER_SIZE]{};
    std::string request_data_;
};

class ProxyServer {
public:
    ProxyServer(asio::io_context& io_context, short port, LoadBalancer& lb, ssl::context& ctx)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
          lb_(lb),
          server_ctx_(ctx) {}

    void Start() {
        DoAccept();
    }

private:
    void DoAccept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), lb_, server_ctx_)->Start();
                }
                DoAccept();
            });
    }

    tcp::acceptor acceptor_;
    LoadBalancer& lb_;
    ssl::context& server_ctx_;
};

int main() {
    try {
        if (!LoadConfig("/etc/defense_cloud/config.enc", "hsm_dec_key")) {
            std::cerr << "Failed to load config" << std::endl;
            return 1;
        }

        asio::io_context io_context;

        LoadBalancer lb;
        for (const auto& addr : g_config.backend_ips) {
            lb.AddBackend(addr);
        }
        lb.StartHealthChecks();

        // Create TLS context
        ssl::context server_ctx = CreateServerContext();

        short port = 443;  // From g_config.lb_listen_addr in production
        ProxyServer server(io_context, port, lb, server_ctx);

        std::cout << "Defense LB Proxy (TLS/mTLS) listening on :" << port << std::endl;

        std::vector<std::thread> threads;
        for (int i = 0; i < std::thread::hardware_concurrency(); ++i) {
            threads.emplace_back([&io_context] { io_context.run(); });
        }
        for (auto& t : threads) t.join();
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}


// || --------- Dependencies (CMake / build) --------- || 

// find_package(Boost REQUIRED COMPONENTS system thread)
// # Beast is header-only but needs Asio + OpenSSL for SSL
// find_package(OpenSSL REQUIRED)
// target_link_libraries(your_target Boost::system Boost::thread OpenSSL::SSL OpenSSL::Crypto)



// // speed/load_balancer/lb_proxy.cpp
// // Defense Private Cloud - Low-Latency HTTP Reverse Proxy / Load Balancer
// // Uses Boost.Asio for async networking

// #include <boost/asio.hpp>
// #include <boost/asio/ssl.hpp>  // optional for TLS
// #include <iostream>
// #include <memory>
// #include <string>
// #include <vector>
// #include <thread>
// #include <atomic>
// #include "lb_core.h"       // From earlier: LoadBalancer, Backend
// #include "config_loader.h" // g_config

// namespace asio = boost::asio;
// using tcp = asio::ip::tcp;

// // Buffer size for reading/writing
// constexpr size_t BUFFER_SIZE = 8192;

// // Session handles one client connection
// class Session : public std::enable_shared_from_this<Session> {
// public:
//     Session(tcp::socket socket, LoadBalancer& lb)
//         : client_socket_(std::move(socket)),
//           strand_(asio::make_strand(client_socket_.get_executor())),
//           lb_(lb) {}

//     void Start() {
//         DoReadRequest();
//     }

// private:
//     void DoReadRequest() {
//         auto self = shared_from_this();
//         client_socket_.async_read_some(
//             asio::buffer(request_buffer_),
//             asio::bind_executor(strand_,
//                 [this, self](boost::system::error_code ec, std::size_t length) {
//                     if (!ec) {
//                         request_data_.append(request_buffer_.data(), length);
//                         // Simple check for end of headers (\r\n\r\n)
//                         if (request_data_.find("\r\n\r\n") != std::string::npos) {
//                             HandleRequest();
//                         } else {
//                             DoReadRequest();  // Continue reading headers
//                         }
//                     } else {
//                         // Error or close
//                     }
//                 }));
//     }

//     void HandleRequest() {
//         // Very basic parsing: extract method, path, host
//         size_t pos = request_data_.find("\r\n");
//         if (pos == std::string::npos) return;
//         std::string request_line = request_data_.substr(0, pos);

//         // Find Host header
//         std::string host;
//         size_t host_pos = request_data_.find("Host: ");
//         if (host_pos != std::string::npos) {
//             size_t end = request_data_.find("\r\n", host_pos);
//             host = request_data_.substr(host_pos + 6, end - host_pos - 6);
//         }

//         // Select backend
//         Backend* backend = lb_.GetNextHealthyBackend();
//         if (!backend || !backend->healthy.load()) {
//             SendError(503, "No healthy backends");
//             return;
//         }

//         // Connect to backend
//         DoConnectToBackend(backend->addr);
//     }

//     void DoConnectToBackend(const std::string& backend_addr) {
//         auto self = shared_from_this();

//         // Parse addr "ip:port"
//         size_t colon = backend_addr.find(':');
//         if (colon == std::string::npos) return;
//         std::string ip = backend_addr.substr(0, colon);
//         std::string port_str = backend_addr.substr(colon + 1);

//         tcp::resolver resolver(client_socket_.get_executor());
//         auto endpoints = resolver.resolve(ip, port_str);

//         asio::async_connect(
//             backend_socket_,
//             endpoints,
//             asio::bind_executor(strand_,
//                 [this, self](boost::system::error_code ec, const tcp::endpoint&) {
//                     if (!ec) {
//                         // Modify request slightly (add X-Forwarded-For)
//                         size_t pos = request_data_.find("\r\n\r\n");
//                         if (pos != std::string::npos) {
//                             std::string forwarded = "X-Forwarded-For: " + client_socket_.remote_endpoint().address().to_string() + "\r\n";
//                             request_data_.insert(pos, forwarded);
//                         }

//                         DoWriteToBackend();
//                     } else {
//                         SendError(502, "Bad Gateway - Backend connect failed");
//                     }
//                 }));
//     }

//     void DoWriteToBackend() {
//         auto self = shared_from_this();
//         asio::async_write(
//             backend_socket_,
//             asio::buffer(request_data_),
//             asio::bind_executor(strand_,
//                 [this, self](boost::system::error_code ec, std::size_t) {
//                     if (!ec) {
//                         DoReadFromBackend();
//                     } else {
//                         // Error
//                     }
//                 }));
//     }

//     void DoReadFromBackend() {
//         auto self = shared_from_this();
//         backend_socket_.async_read_some(
//             asio::buffer(response_buffer_),
//             asio::bind_executor(strand_,
//                 [this, self](boost::system::error_code ec, std::size_t length) {
//                     if (!ec) {
//                         DoWriteToClient(length);
//                     } else if (ec == asio::error::eof) {
//                         // Done
//                     } else {
//                         // Error
//                     }
//                 }));
//     }

//     void DoWriteToClient(std::size_t length) {
//         auto self = shared_from_this();
//         asio::async_write(
//             client_socket_,
//             asio::buffer(response_buffer_, length),
//             asio::bind_executor(strand_,
//                 [this, self](boost::system::error_code ec, std::size_t) {
//                     if (!ec) {
//                         DoReadFromBackend();  // Continue streaming
//                     }
//                 }));
//     }

//     void SendError(int code, const std::string& msg) {
//         std::string response = "HTTP/1.1 " + std::to_string(code) + " " + msg + "\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
//         asio::write(client_socket_, asio::buffer(response));
//         client_socket_.close();
//     }

//     tcp::socket client_socket_;
//     tcp::socket backend_socket_{client_socket_.get_executor()};
//     asio::strand<asio::io_context::executor_type> strand_;
//     LoadBalancer& lb_;
//     char request_buffer_[BUFFER_SIZE]{};
//     char response_buffer_[BUFFER_SIZE]{};
//     std::string request_data_;
// };

// class ProxyServer {
// public:
//     ProxyServer(asio::io_context& io_context, short port, LoadBalancer& lb)
//         : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
//           lb_(lb) {}

//     void Start() {
//         DoAccept();
//     }

// private:
//     void DoAccept() {
//         acceptor_.async_accept(
//             [this](boost::system::error_code ec, tcp::socket socket) {
//                 if (!ec) {
//                     std::make_shared<Session>(std::move(socket), lb_)->Start();
//                 }
//                 DoAccept();
//             });
//     }

//     tcp::acceptor acceptor_;
//     LoadBalancer& lb_;
// };

// int main() {
//     try {
//         // Load config (encrypted, HSM-backed in production)
//         if (!LoadConfig("/etc/defense_cloud/config.enc", "hsm_dec_key")) {
//             std::cerr << "Failed to load config" << std::endl;
//             return 1;
//         }

//         asio::io_context io_context;

//         // Initialize LoadBalancer from config
//         LoadBalancer lb;
//         for (const auto& addr : g_config.backend_ips) {
//             lb.AddBackend(addr);
//         }
//         lb.StartHealthChecks();

//         // Listen on configured address (e.g., 443)
//         short port = 443;  // Parse from g_config.lb_listen_addr
//         ProxyServer server(io_context, port, lb);

//         std::cout << "Defense LB Proxy listening on :" << port << std::endl;

//         // Run multi-threaded for performance
//         std::vector<std::thread> threads;
//         for (int i = 0; i < std::thread::hardware_concurrency(); ++i) {
//             threads.emplace_back([&io_context] { io_context.run(); });
//         }

//         for (auto& t : threads) t.join();
//     } catch (std::exception& e) {
//         std::cerr << "Exception: " << e.what() << std::endl;
//     }

//     return 0;
// }