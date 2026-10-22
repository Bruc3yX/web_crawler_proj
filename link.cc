// fetch_links.cc  --  Person 1
// Needs read.h (https://github.com/ShakaUVM/read) in the same folder.
// Build: g++ -std=c++17 -O2 main.cc fetch_links.cc -o crawler -lssl -lcrypto -pthread
#include "fetch_links.h"
#include "read.h"          // ShakaUVM readlib: readline()

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp = net::ip::tcp;

namespace {

const char* USER_AGENT = "Mozilla/5.0 (compatible; ClassProjectCrawler/1.0; student project)";
const int MAX_REDIRECTS = 5;
const std::size_t MAX_BODY = 8 * 1024 * 1024;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

struct Url { std::string scheme, host, port, target; };

bool parse_url(const std::string& s, Url& u) {
    size_t p = s.find("://");
    if (p == std::string::npos) return false;
    u.scheme = lower(s.substr(0, p));
    if (u.scheme != "http" && u.scheme != "https") return false;
    size_t hs = p + 3;
    size_t pe = s.find_first_of("/?#", hs);
    std::string auth = s.substr(hs, pe == std::string::npos ? std::string::npos : pe - hs);
    std::string rest = (pe == std::string::npos) ? "/" : s.substr(pe);
    size_t h = rest.find('#');
    if (h != std::string::npos) rest.erase(h);
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;
    size_t c = auth.rfind(':');
    if (c != std::string::npos) { u.host = auth.substr(0, c); u.port = auth.substr(c + 1); }
    else { u.host = auth; u.port = (u.scheme == "https") ? "443" : "80"; }
    u.target = rest;
    return !u.host.empty();
}

// Collapse "." and ".." segments, keeping any ?query
std::string remove_dots(const std::string& in) {
    size_t q = in.find('?');
    std::string path = in.substr(0, q);
    std::string query = (q == std::string::npos) ? "" : in.substr(q);
    bool trailing = !path.empty() && path.back() == '/';
    std::vector<std::string> out;
    std::stringstream ss(path);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") { if (!out.empty()) out.pop_back(); }
        else out.push_back(seg);
    }
    std::string r = "/";
    for (size_t i = 0; i < out.size(); i++) { r += out[i]; if (i + 1 < out.size()) r += "/"; }
    if (trailing && !out.empty()) r += "/";
    return r + query;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
        s.replace(p, from.size(), to);
}

// Turn an href found on page `base` into an absolute URL ("" if it isn't a web link)
std::string resolve(const Url& base, std::string href) {
    replace_all(href, "&amp;", "&");
    size_t a = href.find_first_not_of(" \t\r\n"), b = href.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    href = href.substr(a, b - a + 1);
    size_t h = href.find('#');
    if (h != std::string::npos) href.erase(h);
    if (href.empty()) return "";

    std::string head = lower(href.substr(0, 12));
    for (const char* bad : {"mailto:", "javascript:", "tel:", "data:", "sms:"})
        if (head.rfind(bad, 0) == 0) return "";
    if (head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0) return href;

    bool default_port = (base.scheme == "https" && base.port == "443") ||
                        (base.scheme == "http" && base.port == "80");
    std::string origin = base.scheme + "://" + base.host + (default_port ? "" : ":" + base.port);

    if (href.rfind("//", 0) == 0) return base.scheme + ":" + href;
    if (href[0] == '/') return origin + remove_dots(href);

    std::string path = base.target.substr(0, base.target.find('?'));
    if (href[0] == '?') return origin + path + href;
    std::string dir = path.substr(0, path.rfind('/') + 1);
    return origin + remove_dots(dir + href);
}

// Blocking sockets: give them a timeout so one dead server can't hang the crawl
void set_timeouts(int fd) {
    timeval tv{15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

template <class Stream>
void exchange(Stream& s, http::request<http::empty_body>& req,
              http::response_parser<http::string_body>& parser) {
    beast::flat_buffer buf;
    http::write(s, req);
    http::read(s, buf, parser);
}

// One HTTP(S) GET, no redirect handling. Throws on network errors.
http::response<http::string_body> fetch(const Url& u) {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    auto const results = resolver.resolve(u.host, u.port);

    http::request<http::empty_body> req{http::verb::get, u.target, 11};
    req.set(http::field::host, u.host);
    req.set(http::field::user_agent, USER_AGENT);
    req.set(http::field::accept, "text/html");
    req.set(http::field::connection, "close");

    http::response_parser<http::string_body> parser;
    parser.body_limit(MAX_BODY);

    if (u.scheme == "https") {
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), u.host.c_str()))
            throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                                                        net::error::get_ssl_category()));
        stream.set_verify_callback(ssl::host_name_verification(u.host));
        auto& sock = beast::get_lowest_layer(stream);
        sock.connect(results);
        set_timeouts(sock.socket().native_handle());
        stream.handshake(ssl::stream_base::client);
        exchange(stream, req, parser);
        beast::error_code ec;
        stream.shutdown(ec);                 // "stream truncated" is common and harmless
    } else {
        beast::tcp_stream stream(ioc);
        stream.connect(results);
        set_timeouts(stream.socket().native_handle());
        exchange(stream, req, parser);
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    }
    return parser.release();
}

// Pull the href out of every <a ...> tag.
// Uses readlib's readline(stream, '<') to split the page into chunks, one per tag:
// each chunk looks like  a href="...">link text   (everything after a '<').
std::vector<std::string> extract_links(const std::string& html, const Url& base) {
    std::istringstream page(html);
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;

    while (true) {
        std::string chunk = readline(page, '<');
        if (page.fail()) break;                                   // out of input
        if (chunk.size() < 2 || std::tolower((unsigned char)chunk[0]) != 'a' ||
            !std::isspace((unsigned char)chunk[1])) continue;     // not an <a ...> tag

        std::string tag = chunk.substr(0, chunk.find('>'));       // just the tag's attributes
        std::string low = lower(tag);

        size_t h = 0;
        while ((h = low.find("href", h)) != std::string::npos) {
            if (!std::isspace((unsigned char)low[h - 1])) { h += 4; continue; }
            size_t i = h + 4;
            while (i < tag.size() && std::isspace((unsigned char)tag[i])) ++i;
            if (i >= tag.size() || tag[i] != '=') { h += 4; continue; }
            ++i;
            while (i < tag.size() && std::isspace((unsigned char)tag[i])) ++i;
            if (i >= tag.size()) break;

            std::string val;
            if (tag[i] == '"' || tag[i] == '\'') {
                size_t e = tag.find(tag[i], i + 1);
                if (e == std::string::npos) break;
                val = tag.substr(i + 1, e - i - 1);
            } else {
                size_t e = i;
                while (e < tag.size() && !std::isspace((unsigned char)tag[e])) ++e;
                val = tag.substr(i, e - i);
            }
            std::string abs = resolve(base, val);
            if (!abs.empty() && seen.insert(abs).second) out.push_back(abs);
            break;
        }
    }
    return out;
}

} // namespace

std::vector<std::string> get_links(const std::string& start_url) {
    std::string url = start_url;
    try {
        for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
            Url u;
            if (!parse_url(url, u)) return {};
            auto res = fetch(u);
            unsigned code = res.result_int();

            if (code >= 300 && code < 400) {                 // follow redirect
                auto it = res.find(http::field::location);
                if (it == res.end()) return {};
                url = resolve(u, std::string(it->value()));
                if (url.empty()) return {};
                continue;
            }
            if (code != 200) {
                std::cerr << "[get_links] HTTP " << code << " for " << url << "\n";
                return {};
            }
            auto ct = res.find(http::field::content_type);
            if (ct != res.end() && lower(std::string(ct->value())).find("html") == std::string::npos)
                return {};                                   // not a web page (pdf, image, ...)
            return extract_links(res.body(), u);
        }
    } catch (const std::exception& e) {
        std::cerr << "[get_links] " << start_url << ": " << e.what() << "\n";
    }
    return {};
}
