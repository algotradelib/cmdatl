#pragma once

// A loopback HTTPS server for the KIS command tests: a trimmed copy of algotradelib's tests/local_https_server.hpp at
// c917f84 (its test certificate, scripted replies and recorded requests), with one connection served per thread and two
// additions: a hold on a named occurrence of a path, released by the test or by stopping the server, and a wait until
// every connection has closed.

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace support {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// Test-only TLS identity, copied from the library's tests. The private key is intentionally public and exists solely
// to make loopback transport tests deterministic across machines. It must never be used by production or externally
// reachable services.
inline constexpr std::string_view LOCALHOST_CERTIFICATE = R"(-----BEGIN CERTIFICATE-----
MIIDHzCCAgegAwIBAgIUDPB/ppDbPzW9VYCOuxw3EBnOhycwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDkxODA2NDMzMFoXDTM2MDkx
NTA2NDMzMFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAtHKP+PI5U4I9yz8hLBCEsD1cVqPk7r6dy/dLdFMEC9fr
Kmm79cWBjIH1ndyDqhBkQroFcBLF5kHs8BtdPI+ZsXnZLSjE/xPFTe50MonBseyo
qujeOZPLFuxM9zjIji32TOiY8BLlgFeSO4L1d2H6oimTahE4Kl/nf0lA4VX0Hado
8KGQ93hRiWos6OsMSDaXoUJK12v+Qtw0eFU4SZ6+oBAYH6DdHsU3Bowo/x0G9UrL
+RYWlRRYxYB43Qq1hEhJ7woqZ7ZHZdg1T7uOAyW8rtr0sDwU3ggElwxq6mheI1yM
0YST2rBfgumbpD1o35QC85jGf1DOibEenQRwfZzq+wIDAQABo2kwZzAdBgNVHQ4E
FgQUQWa8Q5/3HA76+LO/raAbYjs5PU8wHwYDVR0jBBgwFoAUQWa8Q5/3HA76+LO/
raAbYjs5PU8wDwYDVR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3Qw
DQYJKoZIhvcNAQELBQADggEBAG4BA6iK7+dtPTBlxhv0+5jsPNML0waM5UZYK75k
buE+2zmMRkQHFh3nBP8zk41yTsLIBMW2jc9cbwGvBW68PP/SWerOgKpzGem4kVZu
HV5Q5L+s8J2g4BJtVG0LUGA4nXkaBKTOmhv1TzONSGs+oNOftGiVYa9doUF70Vtu
8fImBiSLrUvHyTjrOrTCQ50MikELOhS+fbryBQoruV/Tam/BAnu6WNV0gi7AT80C
ee2zdl6zhc4Jvech9m9EuN0OKhVWi3fOq1C/aMfiBMC997KrXULdAOLhx8K1sWNA
mavbpV5W8SbPPU77vM2/FBXwjqFUL14KDHoJgyuMNW2UH1o=
-----END CERTIFICATE-----
)";

/** @brief The test certificate's private key; see `LOCALHOST_CERTIFICATE`. */
inline constexpr std::string_view LOCALHOST_PRIVATE_KEY = R"(-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC0co/48jlTgj3L
PyEsEISwPVxWo+Tuvp3L90t0UwQL1+sqabv1xYGMgfWd3IOqEGRCugVwEsXmQezw
G108j5mxedktKMT/E8VN7nQyicGx7Kiq6N45k8sW7Ez3OMiOLfZM6JjwEuWAV5I7
gvV3YfqiKZNqETgqX+d/SUDhVfQdp2jwoZD3eFGJaizo6wxINpehQkrXa/5C3DR4
VThJnr6gEBgfoN0exTcGjCj/HQb1Ssv5FhaVFFjFgHjdCrWESEnvCipntkdl2DVP
u44DJbyu2vSwPBTeCASXDGrqaF4jXIzRhJPasF+C6ZukPWjflALzmMZ/UM6JsR6d
BHB9nOr7AgMBAAECggEAFxafgFFx71L3WCWsN2inqas4QOe4l6mxJBNwxqIGnwNS
FjuwLprGYDWSuU+pUx3Ezlrh7jygsL0XtYMy01hmaHR4StRwHFkpwjZ0DLFepMbs
r+2a3ZAhCL5UfL95veN4o8+RHgq07c6GqfGwOcXCHXSnZYdtgdKCmuFDGXH3icGW
ZQHhmiiypplO11My8sMbeZNbkA976EV7BIqH2edAN92/n3BUzkXNdP4+Mh9Tzua/
80mCHRxnfyITA2vqLyw8ohM3RcBscp4UiCjg3nR4Do+Yr8d0oTUpObCoyeR9bkvi
vZhcmaCwQTYZJ7wnfzIh1tafsioBc52KX3kr3DJulQKBgQDZZ0sjCvkuCsKCzu4D
LYPE2QSkhWCo2ook6lPtRCkdkHssYaAIYQtgLV5Ny12TvfL8nCF7NyzshmkGChLS
cu2RPYdjOWkFqE/SPNCkvuYrW2dFLa9iTqFFCTHUM06R1/stB/nyuKVh5MtxXCVl
d1Z2LK8TzkaJs5sP9977/AODXwKBgQDUe6riXBnhUHdLE4fECKtlydlEN+j9VtDO
0UoL8141s9QCV9b37Cg1ncOInD9eRDGjXq6AFeqUTJk1BLuLHIPddLeZAcT4QY3Y
3L8xZWxC+QFgu+TOgR7HSFAEzQMTHNwlHIAq1Xe5EW4QNwyai4cjg6iroW8GpS3a
1k/Nwu/55QKBgBcMYGbirvA2q9B6Z7lmheXErD1U9p5wLN0lCqjI5SMBQk2+O90n
jLExniHZQZ58MNqfIt2RR+37gf7l6oDtNkqDn2rhl0TGR1AeqnAM3RITqqsm4MUn
WOmcw++fUOEdaL+pbNJSyRXLz9XA0yYitMPqkNUktU28AOB8guNsIGM5AoGAJ25z
veIdo2SNXs9XCJgbRp54xWIiyLDNuzO3rBQTzZ97npuydvvKUnjeioGEePfqxkiR
1Y87ox276MziCOTXy8cXMzvS08dGjZIZoQ8TUjJngQ4dXf00OwM+ZkWtZmSargBm
AIzw1NVZOYUdv6hZAcwzBvbEZdVNdykCUkr0wckCgYABbrErIvrcMwl1yjXDNoXf
QRyuaneIPoOijAwlSPUq7LGrWjViuEaygiu8WFFMWmWzJMe87psGHRhSJ7G8fcmo
6wND1ASNUYmKJAyMDK8ugxFQ5ATHmxt2Lf0cRD+zpmmD4+/3qCoMZucHXFcq89VZ
OV9IxaWtgjumdgBw478W4w==
-----END PRIVATE KEY-----
)";

/**
 * @brief Makes a client context trust the test certificate.
 * @param context The context.
 */
inline void trust_localhost_certificate(ssl::context& context) {
  context.set_verify_mode(ssl::verify_peer);
  context.add_certificate_authority(net::buffer(LOCALHOST_CERTIFICATE));
}

/** @brief One request the server read whole. */
struct Recorded {
  /** @brief The method, as `GET` or `POST`. */
  std::string method_;
  /** @brief The path, without the query. */
  std::string path_;
  /** @brief The query, without the `?`, or empty. */
  std::string query_;
  /** @brief The headers by name, as sent. */
  std::map<std::string, std::string> headers_;
  /** @brief The body. */
  std::string body_;
  /** @brief Where it stood on its connection: 1 for the connection's first request. */
  std::size_t position_{0};
  /** @brief When it was read whole. */
  std::chrono::steady_clock::time_point at_{};

  /**
   * @brief A header's value.
   * @param name The name, as sent.
   * @return The value, or empty.
   */
  std::string header(const std::string& name) const {
    const auto found = headers_.find(name);
    return found == headers_.end() ? std::string() : found->second;
  }

  /**
   * @brief A query parameter's value.
   * @param name The name.
   * @return The value, or nothing when the query has no such parameter.
   */
  std::optional<std::string> param(const std::string& name) const {
    std::size_t at = 0;
    while (at <= query_.size()) {
      const auto end = std::min(query_.find('&', at), query_.size());
      const auto pair = query_.substr(at, end - at);
      const auto equals = pair.find('=');
      if (pair.substr(0, equals) == name) {
        return equals == std::string::npos ? std::string() : pair.substr(equals + 1);
      }
      at = end + 1;
    }
    return std::nullopt;
  }
};

/** @brief One scripted reply: a status, a JSON body and response headers. */
struct Scripted {
  /** @brief The HTTP status. */
  unsigned status_{200};
  /** @brief The body. */
  std::string body_;
  /** @brief Response headers beside the server's own. */
  std::map<std::string, std::string> headers_;
};

/**
 * @brief Answers a request for a path with what its script says, the request and its occurrence (1 for the first
 *   since the path was scripted) in hand.
 */
using Responder = std::function<Scripted(const Recorded&, std::size_t)>;

/**
 * @brief A loopback HTTPS server on an ephemeral port, serving each connection on its own thread.
 *
 * Every request is recorded. A path with no script is answered HTTP 500 with a KIS-shaped error naming it, so a
 * request a test did not expect fails loudly in the client.
 * A hold stops the reply to one named occurrence of a path until the test releases it; stopping the server releases
 * it too, closes every connection and joins every thread, so no test can hang on it.
 */
class LocalHttpsServer {
public:
  /** @brief Starts listening on 127.0.0.1. */
  LocalHttpsServer()
  : context_(ssl::context::tls_server), acceptor_(ioc_, tcp::endpoint(net::ip::address_v4::loopback(), 0)) {
    context_.use_certificate_chain(net::buffer(LOCALHOST_CERTIFICATE));
    context_.use_private_key(net::buffer(LOCALHOST_PRIVATE_KEY), ssl::context::pem);
    acceptor_.non_blocking(true);
    port_ = std::to_string(acceptor_.local_endpoint().port());
    accepting_ = std::thread([this] {
      accept_loop();
    });
  }

  LocalHttpsServer(const LocalHttpsServer&) = delete;
  LocalHttpsServer& operator=(const LocalHttpsServer&) = delete;

  /** @brief Stops the server. */
  ~LocalHttpsServer() {
    stop();
  }

  /**
   * @brief Stops accepting, releases a hold, closes every connection and joins every thread; later calls do
   *   nothing.
   */
  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) {
        return;
      }
      stopped_ = true;
      for (const int fd : open_sockets_) {
        ::shutdown(fd, SHUT_RDWR);
      }
    }
    changed_.notify_all();
    if (accepting_.joinable()) {
      accepting_.join();
    }
    std::vector<std::thread> threads;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      threads.swap(serving_);
    }
    for (auto& thread : threads) {
      thread.join();
    }
    beast::error_code ignored;
    acceptor_.close(ignored);
  }

  /**
   * @brief The port, for a client.
   * @return It, as a string.
   */
  const std::string& port() const noexcept {
    return port_;
  }

  /**
   * @brief Answers every later request for a path with one reply.
   * @param path The path, without a query.
   * @param status The status.
   * @param body The body.
   */
  void script(const std::string& path, unsigned status, std::string body) {
    script_sequence(path, {Scripted{status, std::move(body), {}}});
  }

  /**
   * @brief Answers the requests for a path with replies in order: each takes the next, and the last repeats.
   * @param path The path, without a query.
   * @param replies The replies; at least one.
   */
  void script_sequence(const std::string& path, std::vector<Scripted> replies) {
    respond(path, [replies = std::move(replies)](const Recorded&, std::size_t occurrence) {
      return replies[std::min(occurrence, replies.size()) - 1];
    });
  }

  /**
   * @brief Answers the requests for a path with what a function makes of each.
   * @param path The path, without a query.
   * @param responder The function; it runs on a connection's thread, under no lock of the server's.
   */
  void respond(const std::string& path, Responder responder) {
    std::lock_guard<std::mutex> lock(mutex_);
    scripts_[path] = Script{std::move(responder), 0};
  }

  /**
   * @brief Holds the reply to the `occurrence`-th request for a path whose query holds `fragment`, counted from the
   *   requests that arrive after this call; one hold at a time, replacing any earlier one.
   * @param path The path, without a query.
   * @param occurrence Which matching request, from 1.
   * @param fragment Text the query must hold, or empty for any.
   */
  void hold(const std::string& path, std::size_t occurrence, const std::string& fragment = "") {
    std::lock_guard<std::mutex> lock(mutex_);
    hold_ = Hold{path, fragment, occurrence, 0, false, false};
  }

  /**
   * @brief Waits until the held request has arrived.
   * @param timeout The longest wait.
   * @return Whether it arrived.
   */
  bool wait_held(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] {
      return hold_ && hold_->arrived_;
    });
  }

  /** @brief Lets the held request's reply go, or the next matching one if it has not arrived; ends the hold. */
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (hold_) {
        hold_->released_ = true;
      }
    }
    changed_.notify_all();
  }

  /**
   * @brief Waits until every connection has closed: a connection whose client is gone is closed by its thread after
   *   the reply it was making, so a script that reply ran has finished.
   * @param timeout The longest wait.
   * @return Whether every connection closed.
   */
  bool wait_closed(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] {
      return open_sockets_.empty();
    });
  }

  /**
   * @brief Every request read so far, in arrival order.
   * @return The requests.
   */
  std::vector<Recorded> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

  /**
   * @brief The requests for a path read so far, in arrival order.
   * @param path The path, without a query.
   * @return The requests.
   */
  std::vector<Recorded> requests(const std::string& path) const {
    std::vector<Recorded> out;
    for (auto& request : requests()) {
      if (request.path_ == path) {
        out.push_back(std::move(request));
      }
    }
    return out;
  }

  /** @brief Forgets the requests recorded so far; the scripts' counts stay. */
  void clear_requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.clear();
  }

private:
  /** @brief A path's script and how many requests it has answered. */
  struct Script {
    /** @brief The function. */
    Responder responder_;
    /** @brief The requests answered. */
    std::size_t answered_{0};
  };

  /** @brief The hold. */
  struct Hold {
    /** @brief The path. */
    std::string path_;
    /** @brief Text the query must hold. */
    std::string fragment_;
    /** @brief Which matching request. */
    std::size_t occurrence_{1};
    /** @brief Matching requests seen. */
    std::size_t seen_{0};
    /** @brief Whether the held request arrived. */
    bool arrived_{false};
    /** @brief Whether the test let it go. */
    bool released_{false};
  };

  /** @brief Accepts until stopped, starting a thread for each connection. */
  void accept_loop() noexcept {
    try {
      while (true) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (stopped_) {
            return;
          }
        }
        pollfd waiting{acceptor_.native_handle(), POLLIN, 0};
        if (::poll(&waiting, 1, 20) <= 0) {
          continue;
        }
        tcp::socket socket(ioc_);
        beast::error_code ec;
        acceptor_.accept(socket, ec);
        if (ec) {
          continue;
        }
        socket.non_blocking(false, ec);
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
          return;
        }
        open_sockets_.insert(socket.native_handle());
        serving_.emplace_back([this, raw = std::make_shared<tcp::socket>(std::move(socket))]() mutable {
          serve(std::move(*raw));
        });
      }
    } catch (...) {
    }
  }

  /**
   * @brief Serves one connection until the client closes it or the server stops; a blocked read ends when the server
   *   stops, which shuts the socket down.
   * @param socket The connection.
   */
  void serve(tcp::socket socket) noexcept {
    const int fd = socket.native_handle();
    try {
      beast::ssl_stream<tcp::socket> stream(std::move(socket), context_);
      beast::error_code ec;
      stream.handshake(ssl::stream_base::server, ec);
      beast::flat_buffer buffer;
      std::size_t position = 0;
      while (!ec && !stopping()) {
        http::request<http::string_body> request;
        http::read(stream, buffer, request, ec);
        if (ec) {
          break;
        }
        const auto reply = answer(request, ++position);
        if (!reply) {
          break;
        }
        http::response<http::string_body> response(static_cast<http::status>(reply->status_), request.version());
        response.set(http::field::server, "cmdatl-test");
        response.set(http::field::content_type, "application/json;charset=UTF-8");
        for (const auto& [name, value] : reply->headers_) {
          response.set(name, value);
        }
        response.keep_alive(request.keep_alive());
        response.body() = reply->body_;
        response.prepare_payload();
        http::write(stream, response, ec);
        if (!request.keep_alive()) {
          break;
        }
      }
      forget(fd);
      beast::error_code ignored;
      stream.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
      stream.next_layer().close(ignored);
    } catch (...) {
      forget(fd);
    }
  }

  /**
   * @brief Whether the server is stopping.
   * @return Whether it is.
   */
  bool stopping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
  }

  /**
   * @brief Records a request, waits out a hold on it, and makes its reply with the path's script as it then stands.
   * @param request The request.
   * @param position Where it stood on the connection.
   * @return The reply, or nothing when the server stopped while it was held.
   */
  std::optional<Scripted> answer(const http::request<http::string_body>& request, std::size_t position) {
    Recorded recorded;
    recorded.method_ = std::string(request.method_string());
    const std::string target(request.target());
    const auto mark = target.find('?');
    recorded.path_ = target.substr(0, mark);
    recorded.query_ = mark == std::string::npos ? std::string() : target.substr(mark + 1);
    for (const auto& field : request) {
      recorded.headers_[std::string(field.name_string())] = std::string(field.value());
    }
    recorded.body_ = request.body();
    recorded.position_ = position;
    recorded.at_ = std::chrono::steady_clock::now();
    Responder responder;
    std::size_t occurrence = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      requests_.push_back(recorded);
      if (const auto found = scripts_.find(recorded.path_); found != scripts_.end()) {
        occurrence = ++found->second.answered_;
      }
      if (
        hold_ && !hold_->released_ && hold_->path_ == recorded.path_ &&
        recorded.query_.find(hold_->fragment_) != std::string::npos && ++hold_->seen_ == hold_->occurrence_
      ) {
        hold_->arrived_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] {
          return stopped_ || hold_->released_;
        });
        if (stopped_) {
          return std::nullopt;
        }
      }
      // The script as it stands when the reply is made: a test may change it while the request is held.
      if (const auto found = scripts_.find(recorded.path_); found != scripts_.end()) {
        responder = found->second.responder_;
      }
    }
    if (!responder) {
      return Scripted{
        500, R"({"rt_cd":"1","msg_cd":"UNSCRIPTED","msg1":"no script for )" + recorded.path_ + R"("})", {}
      };
    }
    return responder(recorded, occurrence);
  }

  /**
   * @brief Takes a connection out of the open set before it is closed, so `stop` never shuts down a descriptor
   *   number reused by something else.
   * @param fd Its descriptor.
   */
  void forget(int fd) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_sockets_.erase(fd);
    }
    changed_.notify_all();
  }

  net::io_context ioc_;
  ssl::context context_;
  tcp::acceptor acceptor_;
  std::string port_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopped_{false};
  std::set<int> open_sockets_;
  std::map<std::string, Script> scripts_;
  std::optional<Hold> hold_;
  std::vector<Recorded> requests_;
  std::thread accepting_;
  std::vector<std::thread> serving_;
};

} // namespace support
