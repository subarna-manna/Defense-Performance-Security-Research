// speed/load_balancer/lb_proxy_beast.cpp
// Defense Private Cloud - HTTPS Reverse Proxy / Load Balancer using Boost.Beast

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include "lb_core.h"       // LoadBalancer, Backend struct
#include "config_loader.h" // g_config

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = boost::asio::ssl;
using tcp       = net::ip::tcp;

// Shared SSL context factory (mTLS capable)
ssl::context create_server_context() {
    ssl::context ctx{ssl::context::tlsv13_server};

    ctx.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2 |
        ssl::context::no_sslv3 |
        ssl::context::no_tlsv1 |
        ssl::context::no_tlsv1_1 |
        ssl::context::single_dh_use);

    ctx.use_certificate_chain_file(g_config.server_cert_path);
    ctx.use_private_key_file(g_config.server_key_path, ssl::context::pem);

    if (!g_config.dh_params_path.empty())
        ctx.use_tmp_dh_file(g_config.dh_params_path);

    if (g_config.enforce_mtls) {
        ctx.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);
        ctx.load_verify_file(g_config.ca_cert_path);
    } else {
        ctx.set_verify_mode(ssl::verify_none);
    }

    return ctx;
}

// One session per client connection
class Session : public std::enable_shared_from_this<Session> {
    beast::tcp_stream          client_stream_;
    ssl::stream<beast::tcp_stream> ssl_stream_;  // wrapped if TLS
    bool                       is_tls_;
    beast::flat_buffer         buffer_;
    http::request<http::string_body> req_;       // we'll read into dynamic_body later if needed
    LoadBalancer&              lb_;

public:
    // ctor for plain (not used here, but kept for flex)
    explicit Session(tcp::socket&& socket, LoadBalancer& lb)
        : client_stream_(std::move(socket)),
          is_tls_(false),
          lb_(lb) {}

    // ctor for TLS (used in this impl)
    Session(tcp::socket&& socket, ssl::context& ctx, LoadBalancer& lb)
        : client_stream_(std::move(socket)),
          ssl_stream_(client_stream_.release_socket(), ctx),
          is_tls_(true),
          lb_(lb) {}

    void run() {
        if (is_tls_) {
            ssl_stream_.async_handshake(
                ssl::stream_base::server,
                beast::bind_front_handler(&Session::on_handshake, shared_from_this()));
        } else {
            do_read();
        }
    }

private:
    void on_handshake(beast::error_code ec) {
        if (ec) {
            std::cerr << "Handshake failed: " << ec.message() << "\n";
            return;
        }
        do_read();
    }

    void do_read() {
        req_ = {};
        auto& stream = is_tls_ ? ssl_stream_ : client_stream_;

        http::async_read(stream, buffer_, req_,
            beast::bind_front_handler(&Session::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec == http::error::end_of_stream) {
            shutdown();
            return;
        }
        if (ec) {
            std::cerr << "Read error: " << ec.message() << "\n";
            return;
        }

        // Process request
        forward_request();
    }

    void forward_request() {
        Backend* backend = lb_.GetNextHealthyBackend();
        if (!backend || !backend->healthy.load()) {
            send_error(http::status::service_unavailable, "No healthy backends");
            return;
        }

        // Parse backend addr "ip:port"
        size_t colon = backend->addr.find(':');
        if (colon == std::string::npos) {
            send_error(http::status::bad_gateway, "Invalid backend");
            return;
        }
        std::string host = backend->addr.substr(0, colon);
        std::string port = backend->addr.substr(colon + 1);

        // Prepare forwarded request
        http::request<http::string_body> forwarded_req = req_;  // copy
        forwarded_req.target(req_.target());                    // keep original path
        forwarded_req.set(http::field::host, host + ":" + port);
        forwarded_req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING " (Defense Proxy)");
        forwarded_req.set(http::field::connection, "close");    // simplify for now

        // Add proxy headers
        auto client_ip = client_stream_.socket().remote_endpoint().address().to_string();
        forwarded_req.set("X-Forwarded-For", client_ip);
        forwarded_req.set("X-Forwarded-Proto", is_tls_ ? "https" : "http");

        // Connect to backend (plain HTTP assumed; add ssl_stream for backend if needed)
        net::io_context ioc;  // per-request io_context (or reuse shared one)
        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(host, port);

        beast::tcp_stream backend_stream(ioc);
        backend_stream.connect(results);

        // Send forwarded request
        http::write(backend_stream, forwarded_req);

        // Read response from backend
        beast::flat_buffer backend_buffer;
        http::response<http::string_body> res;
        http::read(backend_stream, backend_buffer, res);

        // Forward response to client
        auto& client = is_tls_ ? ssl_stream_ : client_stream_;
        http::async_write(client, res,
            beast::bind_front_handler(&Session::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "Write error: " << ec.message() << "\n";
            return;
        }
        // For keep-alive: loop back to do_read()
        // For simplicity: close after each request
        shutdown();
    }

    void send_error(http::status status, std::string msg) {
        http::response<http::string_body> res{status, req_.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/plain");
        res.body() = msg;
        res.prepare_payload();

        auto& stream = is_tls_ ? ssl_stream_ : client_stream_;
        http::async_write(stream, res,
            [self = shared_from_this()](beast::error_code, std::size_t){ self->shutdown(); });
    }

    void shutdown() {
        beast::error_code ec;
        if (is_tls_) {
            ssl_stream_.async_shutdown([self = shared_from_this()](auto){});
        } else {
            client_stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
        }
    }
};

// Acceptor
class Listener : public std::enable_shared_from_this<Listener> {
    net::io_context&       ioc_;
    ssl::context&          ctx_;
    tcp::acceptor          acceptor_;
    LoadBalancer&          lb_;

public:
    Listener(net::io_context& ioc, ssl::context& ctx, tcp::endpoint endpoint, LoadBalancer& lb)
        : ioc_(ioc),
          ctx_(ctx),
          acceptor_(net::make_strand(ioc), endpoint),
          lb_(lb) {}

    void run() { do_accept(); }

private:
    void do_accept() {
        acceptor_.async_accept(
            net::make_strand(ioc_),
            beast::bind_front_handler(&Listener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<Session>(std::move(socket), ctx_, lb_)->run();
        }
        do_accept();
    }
};

int main() {
    try {
        if (!LoadConfig("/etc/defense_cloud/config.enc", "hsm_dec_key")) {
            std::cerr << "Config load failed\n";
            return 1;
        }

        net::io_context ioc{std::thread::hardware_concurrency()};

        LoadBalancer lb;
        for (const auto& addr : g_config.backend_ips) {
            lb.AddBackend(addr);
        }
        lb.StartHealthChecks();

        ssl::context server_ctx = create_server_context();

        auto const address = net::ip::make_address("0.0.0.0");
        unsigned short port = 443;

        std::make_shared<Listener>(ioc, server_ctx,
            tcp::endpoint{address, port}, lb)->run();

        std::cout << "Defense LB Proxy (Beast + TLS/mTLS) listening on :" << port << "\n";

        std::vector<std::thread> threads;
        for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i) {
            threads.emplace_back([&ioc] { ioc.run(); });
        }
        for (auto& t : threads) t.join();
    }
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

// find_package(Boost REQUIRED COMPONENTS system thread)
// # Beast is header-only but needs Asio + OpenSSL for SSL
// find_package(OpenSSL REQUIRED)
// target_link_libraries(your_target Boost::system Boost::thread OpenSSL::SSL OpenSSL::Crypto)

// curl -v --cacert ca.pem --cert client-cert.pem --key client-key.pem https://your-proxy:443/



