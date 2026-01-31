// data/object_storage/s3_api_server.h
#ifndef S3_API_SERVER_H
#define S3_API_SERVER_H

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include "storage_manager.h"

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

class S3ApiSession : public std::enable_shared_from_this<S3ApiSession> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    StorageManager& storage_;

public:
    S3ApiSession(tcp::socket&& socket, StorageManager& storage)
        : stream_(std::move(socket)), storage_(storage) {}

    void run();

private:
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void handle_request();
    void send_response(http::response<http::string_body>&& res);
    void send_error(http::status status, const std::string& msg);

    // Operation handlers
    void handle_put_object(const std::string& bucket, const std::string& key);
    void handle_get_object(const std::string& bucket, const std::string& key);
    void handle_delete_object(const std::string& bucket, const std::string& key);
    void handle_list_objects_v2(const std::string& bucket);

    std::string generate_list_xml(const std::vector<std::string>& keys,
                                  const std::string& prefix, const std::string& continuation,
                                  size_t max_keys) const;
};

class S3ApiServer {
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    StorageManager& storage_;

public:
    S3ApiServer(net::io_context& ioc, tcp::endpoint endpoint, StorageManager& storage);

    void run();
};

#endif
