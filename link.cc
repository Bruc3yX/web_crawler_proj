/*
  Downloads one page and returns the http(s) links found in its <a href> tags
  as full URLs, with no duplicates. If anything goes wrong (bad URL, timeout,
  404, not HTML) it returns an empty vector, so the crawler can call it
  without any try/catch of its own.

  How it's laid out:
    parse_url()     split a URL into scheme / host / port / path
    resolve()       turn an href like "/a" or "../b" into a full URL
    fetch()         one HTTP(S) GET with Boost.Beast, with timeouts
    extract_links() use readlib's readline(page, '<') to go tag by tag
    get_links()     fetch, follow redirects, check it's HTML, extract
*/
#include "fetch_links.h"
#include "read.h"          // ShakaUVM readlib: readline(stream, delimiter)

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
using namespace std;
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
namespace {
const char* USER_AGENT = "Mozilla/5.0 (compatible; ClassProjectCrawler/1.0; student project)";
const int MAX_REDIRECTS = 5;
const size_t MAX_BODY = 8 * 1024 * 1024;        // skip pages over 8 MB
const auto TIMEOUT = chrono::seconds(15);
string to_lower(string text) {
	transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
		return tolower(ch);
	});
	return text;
}

// ---- splitting up a URL ----
struct Url {
	string scheme, host, port, target;   // target = path + ?query, always starts with '/'
};

bool parse_url(const string& url_text, Url& parts) {
	size_t scheme_end = url_text.find("://");
	if (scheme_end == string::npos) return false;
	parts.scheme = to_lower(url_text.substr(0, scheme_end));
	if (parts.scheme != "http" && parts.scheme != "https") return false;
	size_t host_start = scheme_end + 3;
	size_t host_end = url_text.find_first_of("/?#", host_start);                       // where host[:port] ends
	string authority = url_text.substr(host_start, host_end == string::npos ? string::npos : host_end - host_start);
	string path_and_query = (host_end == string::npos) ? "/" : url_text.substr(host_end);
	size_t hash_pos = path_and_query.find('#');
	if (hash_pos != string::npos) path_and_query.erase(hash_pos);                    // the #fragment never goes to the server
	if (path_and_query.empty() || path_and_query[0] != '/') path_and_query = "/" + path_and_query;
	size_t colon_pos = authority.rfind(':');
	if (colon_pos != string::npos) {
		parts.host = to_lower(authority.substr(0, colon_pos));
		parts.port = authority.substr(colon_pos + 1);
	} else {
		parts.host = to_lower(authority);
		parts.port = (parts.scheme == "https") ? "443" : "80";
	}
	if (parts.port.empty()) parts.port = (parts.scheme == "https") ? "443" : "80";
	parts.target = path_and_query;
	return !parts.host.empty();
}

// ---- making links absolute ----

// Handles "." and ".." in a path, keeps the ?query as is.
string remove_dots(const string& input) {
	size_t query_start = input.find('?');
	string path = input.substr(0, query_start);
	string query = (query_start == string::npos) ? "" : input.substr(query_start);
	bool ends_with_slash = !path.empty() && path.back() == '/';
	vector<string> segments;
	stringstream path_stream(path);
	string segment;
	while (getline(path_stream, segment, '/')) {
		if (segment.empty() || segment == ".") continue;
		if (segment == "..") {
			if (!segments.empty()) segments.pop_back();
		} else segments.push_back(segment);
	}
	string result = "/";
	for (size_t i = 0; i < segments.size(); i++) {
		result += segments[i];
		if (i + 1 < segments.size()) result += "/";
	}
	if (ends_with_slash && !segments.empty()) result += "/";
	return result + query;
}

void replace_all(string& text, const string& old_text, const string& new_text) {
	for (size_t pos = 0; (pos = text.find(old_text, pos)) != string::npos; pos += new_text.size())
		text.replace(pos, old_text.size(), new_text);
}

// True if href starts with a scheme like "mailto:" or "https:"
// (letters/digits/+/-/. followed by a ':')
bool has_scheme(const string& href) {
	if (href.empty() || !isalpha((unsigned char)href[0])) return false;
	for (size_t i = 1; i < href.size(); i++) {
		unsigned char ch = href[i];
		if (ch == ':') return true;
		if (!(isalnum(ch) || ch == '+' || ch == '-' || ch == '.')) return false;
	}
	return false;
}

// Turns an href found on page `base` into a full http(s) URL.
// Returns "" for anything that isn't a web link (mailto:, javascript:, "#top", ...)
string resolve(const Url& base, string href) {
	replace_all(href, "&amp;", "&");
	size_t first_non_space = href.find_first_not_of(" \t\r\n"), last_non_space = href.find_last_not_of(" \t\r\n");
	if (first_non_space == string::npos) return "";
	href = href.substr(first_non_space, last_non_space - first_non_space + 1);
	size_t hash_pos = href.find('#');
	if (hash_pos != string::npos) href.erase(hash_pos);
	if (href.empty()) return "";
	replace_all(href, " ", "%20");                                // a raw space would break the request

	if (has_scheme(href)) {                                       // already absolute, keep only http(s)
		string href_lower = to_lower(href);
		if (href_lower.rfind("http://", 0) == 0 || href_lower.rfind("https://", 0) == 0) {
			Url parsed;
			if (!parse_url(href, parsed)) return "";
			bool is_default_port = (parsed.scheme == "https" && parsed.port == "443") || (parsed.scheme == "http" && parsed.port == "80");
			return parsed.scheme + "://" + parsed.host + (is_default_port ? "" : ":" + parsed.port) + parsed.target;
		}
		return "";                                                // mailto:, javascript:, ftp:, tel: ...
	}

	// relative link, so build it from the address of the page it was on
	bool default_port = (base.scheme == "https" && base.port == "443") ||
	                    (base.scheme == "http" && base.port == "80");
	string origin = base.scheme + "://" + base.host + (default_port ? "" : ":" + base.port);

	if (href.rfind("//", 0) == 0) return base.scheme + ":" + href;       // "//cdn.site.com/x"
	if (href[0] == '/') return origin + remove_dots(href);               // "/about"
	string path = base.target.substr(0, base.target.find('?'));
	if (href[0] == '?') return origin + path + href;                     // "?page=2"
	string dir = path.substr(0, path.rfind('/') + 1);
	return origin + remove_dots(dir + href);                             // "page2.html", "../x"
}

// About the timeouts: tcp_stream::expires_after() only works on async operations,
// and setting SO_RCVTIMEO on the socket doesn't help with Asio's blocking calls
// (Asio just waits again). So everything below is async, and run_op() runs the
// io_context until the one operation finishes. That way fetch() still reads top to bottom.

// Start one async operation, wait for it, and throw if it failed or timed out.
template <class StartFn>
void run_op(net::io_context& ioc, StartFn start) {
	beast::error_code ec;
	ioc.restart();
	start([&ec](beast::error_code err, auto&&...) {
		ec = err;
	});
	ioc.run();
	if (ec) throw beast::system_error(ec);
}

// Only build the TLS context once, loading the system certificates is slow.
ssl::context& tls_context() {
	static ssl::context ctx = [] {
		ssl::context new_ctx(ssl::context::tls_client);
		new_ctx.set_default_verify_paths();                 // use the OS certificate store
		new_ctx.set_verify_mode(ssl::verify_peer);          // reject bad certificates
		return new_ctx;
	}();
	return ctx;
}

// Send the request and read the whole response. Works for both plain and TLS streams.
template <class Stream>
void send_and_receive(net::io_context& ioc, Stream& stream, http::request<http::empty_body>& req,
              http::response_parser<http::string_body>& parser) {
	beast::flat_buffer buf;
	auto& socket_layer = beast::get_lowest_layer(stream);

	socket_layer.expires_after(TIMEOUT);
	run_op(ioc, [&](auto handler) {
		http::async_write(stream, req, handler);
	});

	socket_layer.expires_after(TIMEOUT);                      // this one covers reading the whole page
	beast::error_code read_error;
	ioc.restart();
	http::async_read(stream, buf, parser, [&](beast::error_code err, size_t) {
		read_error = err;
	});
	ioc.run();
	// Some servers have no Content-Length and just close the socket when the page is done.
	// Over TLS that comes back as "stream truncated" even though we have the whole page,
	// so tell the parser we hit the end and only keep the error if the page is incomplete.
	if (read_error == net::ssl::error::stream_truncated) {
		beast::error_code eof_error;
		parser.put_eof(eof_error);
		read_error = eof_error;
	}
	if (read_error) throw beast::system_error(read_error);
}

// One GET request. Doesn't follow redirects (get_links does that). Throws if the network fails.
http::response<http::string_body> fetch(const Url& url) {
	net::io_context ioc;
	tcp::resolver resolver(ioc);
	auto const results = resolver.resolve(url.host, url.port);   // DNS lookup, no timeout of our own here

	http::request<http::empty_body> req{http::verb::get, url.target, 11};
	req.set(http::field::host, url.host);
	req.set(http::field::user_agent, USER_AGENT);
	req.set(http::field::accept, "text/html");
	req.set(http::field::connection, "close");       // one request per connection, keeps it simple
	// no Accept-Encoding header, so the server sends plain uncompressed text

	http::response_parser<http::string_body> parser;
	parser.body_limit(MAX_BODY);

	if (url.scheme == "https") {
		beast::ssl_stream<beast::tcp_stream> stream(ioc, tls_context());
		// SNI, tells the server which hostname we want (most sites need this)
		if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()))
			throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
			                          net::error::get_ssl_category()));
		// check the certificate is really for this host
		stream.set_verify_callback(ssl::host_name_verification(url.host));

		auto& socket_layer = beast::get_lowest_layer(stream);
		socket_layer.expires_after(TIMEOUT);
		run_op(ioc, [&](auto handler) {
			socket_layer.async_connect(results, handler);
		});
		socket_layer.expires_after(TIMEOUT);
		run_op(ioc, [&](auto handler) {
			stream.async_handshake(ssl::stream_base::client, handler);
		});
		send_and_receive(ioc, stream, req, parser);
		// no TLS shutdown on purpose, the blocking version can hang on a bad server
		// and we're finished with the connection anyway
	} else {
		beast::tcp_stream stream(ioc);
		stream.expires_after(TIMEOUT);
		run_op(ioc, [&](auto handler) {
			stream.async_connect(results, handler);
		});
		send_and_receive(ioc, stream, req, parser);
	}
	return parser.release();
}

// ---- finding the links in the HTML ----

// readline(page, '<') gives back everything after one '<' up to the next '<',
// so each call is one tag, like:   a href="/about">About us
// We keep the ones that start with "a" and a space (<a ...>), cut it off at the
// first '>' to get just the attributes, then look for href="..." in there.
vector<string> extract_links(const string& html, const Url& base) {
	istringstream page(html);
	vector<string> found;
	unordered_set<string> seen;

	while (true) {
		string chunk = readline(page, '<');
		if (page.fail()) break;
		if (chunk.size() < 2 || tolower((unsigned char)chunk[0]) != 'a' ||
		        !isspace((unsigned char)chunk[1])) continue;      // not an <a ...> tag

		string tag = chunk.substr(0, chunk.find('>'));
		string tag_lower = to_lower(tag);

		size_t href_pos = 0;
		while ((href_pos = tag_lower.find("href", href_pos)) != string::npos) {
			// has to be the attribute "href" itself, not something like "data-href"
			if (href_pos == 0 || !isspace((unsigned char)tag_lower[href_pos - 1])) {
				href_pos += 4;
				continue;
			}
			size_t cursor = href_pos + 4;
			while (cursor < tag.size() && isspace((unsigned char)tag[cursor])) ++cursor;
			if (cursor >= tag.size() || tag[cursor] != '=') {
				href_pos += 4;
				continue;
			}
			++cursor;
			while (cursor < tag.size() && isspace((unsigned char)tag[cursor])) ++cursor;
			if (cursor >= tag.size()) break;

			string val;
			if (tag[cursor] == '"' || tag[cursor] == '\'') {                 // href="..." or href='...'
				size_t end_pos = tag.find(tag[cursor], cursor + 1);
				if (end_pos == string::npos) break;
				val = tag.substr(cursor + 1, end_pos - cursor - 1);
			} else {                                               // href=unquoted
				size_t end_pos = cursor;
				while (end_pos < tag.size() && !isspace((unsigned char)tag[end_pos])) ++end_pos;
				val = tag.substr(cursor, end_pos - cursor);
			}
			string absolute = resolve(base, val);
			if (!absolute.empty() && seen.insert(absolute).second) found.push_back(absolute);
			break;
		}
	}
	return found;
}

} // namespace


// ---- the function everyone else calls ----

vector<string> get_links(const string& start_url) {
	string url = start_url;
	try {
		for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
			Url parts;
			if (!parse_url(url, parts)) {
				cerr << "[get_links] not a valid http(s) URL: " << url << "\n";
				return {};
			}
			auto response = fetch(parts);
			unsigned status = response.result_int();

			if (status >= 300 && status < 400) {                 // redirect, go to the Location header
				auto location_it = response.find(http::field::location);
				if (location_it == response.end()) {
					cerr << "[get_links] HTTP " << status << " redirect with no Location: " << url << "\n";
					return {};
				}
				url = resolve(parts, string(location_it->value()));  // Location can be relative too
				if (url.empty()) {
					cerr << "[get_links] unusable redirect target from HTTP " << status << "\n";
					return {};
				}
				continue;
			}
			if (status != 200) {
				cerr << "[get_links] HTTP " << status << " for " << url << "\n";
				return {};
			}
			auto type_header = response.find(http::field::content_type);
			if (type_header != response.end() && to_lower(string(type_header->value())).find("html") == string::npos) {
				cerr << "[get_links] skipping non-HTML (" << string(type_header->value()) << "): " << url << "\n";
				return {};                                   // pdf, image, etc.
			}
			auto links = extract_links(response.body(), parts);       // u is the final URL after redirects, so relative links resolve right
			if (links.empty())
				cerr << "[get_links] HTTP 200 but no links found (" << response.body().size()
				     << " bytes) at " << url << "\n";
			return links;
		}
		cerr << "[get_links] too many redirects: " << start_url << "\n";
	} catch (const exception& error) {
		cerr << "[get_links] " << start_url << ": " << error.what() << "\n";
	}
	return {};
}
