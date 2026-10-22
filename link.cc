/*
 The public API is declared in fetch_links.h:
    vector<string> get_links(const string& url);
    absolute, de-duplicated http(s) URLs found in <a href="..."> tags.
    returns an EMPTY vector on any failure (bad URL, timeout, 404, not HTML...).
    It never throws, so the crawler can call it blindly.

The flow for how everything works:
   1. parse_url()     split "https://host:port/path?x" into scheme/host/port/target
   2. resolve()       turn any href ("/a", "../b", "//cdn.x.com/c") into an absolute URL
   3. fetch()         ONE HTTP(S) GET using Boost.Beast (tcp_stream / ssl_stream),
                      with a timeout on every network step
   4. extract_links() use readlib's readline(page, '<') to chop the HTML into
                      one chunk per tag, keep the <a ...> ones, pull out href=
   5. get_links()     glue: fetch -> follow redirects -> check it's HTML -> extract
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
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp = net::ip::tcp;
namespace {
const char* USER_AGENT = "Mozilla/5.0 (compatible; ClassProjectCrawler/1.0; student project)";
const int MAX_REDIRECTS = 5;                         // give up after this many 3xx hops
const size_t MAX_BODY = 8 * 1024 * 1024;        // refuse pages bigger than 8 MB
const auto TIMEOUT = chrono::seconds(15);       // per network step (connect/handshake/send/receive)
string lower(string s) {
	transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return tolower(c);
	});
	return s;
}

//URL parsing
struct Url {
	string scheme, host, port, target;   // target = path + ?query (always starts with '/')
};

bool parse_url(const string& s, Url& u) {
	size_t p = s.find("://");
	if (p == string::npos) return false;
	u.scheme = lower(s.substr(0, p));
	if (u.scheme != "http" && u.scheme != "https") return false;
	size_t hs = p + 3;
	size_t pe = s.find_first_of("/?#", hs);                       // end of "host[:port]"
	string auth = s.substr(hs, pe == string::npos ? string::npos : pe - hs);
	string rest = (pe == string::npos) ? "/" : s.substr(pe);
	size_t h = rest.find('#');
	if (h != string::npos) rest.erase(h);                    // fragments are never sent to servers
	if (rest.empty() || rest[0] != '/') rest = "/" + rest;
	size_t c = auth.rfind(':');
	if (c != string::npos) {
		u.host = lower(auth.substr(0, c));                        // hostnames are case-insensitive
		u.port = auth.substr(c + 1);
	} else {
		u.host = lower(auth);
		u.port = (u.scheme == "https") ? "443" : "80";
	}
	if (u.port.empty()) u.port = (u.scheme == "https") ? "443" : "80";
	u.target = rest;
	return !u.host.empty();
}

// Making links absolute
// Collapse "." and ".." segments, keeping any ?query.
string remove_dots(const string& in) {
	size_t q = in.find('?');
	string path = in.substr(0, q);
	string query = (q == string::npos) ? "" : in.substr(q);
	bool trailing = !path.empty() && path.back() == '/';
	vector<string> out;
	stringstream ss(path);
	string seg;
	while (getline(ss, seg, '/')) {
		if (seg.empty() || seg == ".") continue;
		if (seg == "..") {
			if (!out.empty()) out.pop_back();
		} else out.push_back(seg);
	}
	string r = "/";
	for (size_t i = 0; i < out.size(); i++) {
		r += out[i];
		if (i + 1 < out.size()) r += "/";
	}
	if (trailing && !out.empty()) r += "/";
	return r + query;
}

void replace_all(string& s, const string& from, const string& to) {
	for (size_t p = 0; (p = s.find(from, p)) != string::npos; p += to.size())
		s.replace(p, from.size(), to);
}

// True if href starts with "<scheme>:" (e.g. mailto:, ftp:, javascript:, https:).
// A scheme is a letter followed by letters/digits/+/-/. and then ':' before any '/', '?'.
bool has_scheme(const string& href) {
	if (href.empty() || !isalpha((unsigned char)href[0])) return false;
	for (size_t i = 1; i < href.size(); i++) {
		unsigned char c = href[i];
		if (c == ':') return true;
		if (!(isalnum(c) || c == '+' || c == '-' || c == '.')) return false;
	}
	return false;
}

// Turn an href found on page `base` into an absolute http(s) URL.
// Returns "" if it isn't a web link (mailto:, javascript:, ftp:, empty, "#top", ...)
string resolve(const Url& base, string href) {
	replace_all(href, "&amp;", "&");                              // HTML-escaped ampersands
	size_t a = href.find_first_not_of(" \t\r\n"), b = href.find_last_not_of(" \t\r\n");
	if (a == string::npos) return "";
	href = href.substr(a, b - a + 1);                             // trim whitespace
	size_t h = href.find('#');
	if (h != string::npos) href.erase(h);                    // drop #fragment
	if (href.empty()) return "";
	replace_all(href, " ", "%20");                                // raw spaces would break the request line

	if (has_scheme(href)) {                                       // absolute: only keep http / https
		string low = lower(href);
		if (low.rfind("http://", 0) == 0 || low.rfind("https://", 0) == 0) {
			Url u;                                                // re-build so host/scheme are lowercase
			if (!parse_url(href, u)) return "";
			bool def = (u.scheme == "https" && u.port == "443") || (u.scheme == "http" && u.port == "80");
			return u.scheme + "://" + u.host + (def ? "" : ":" + u.port) + u.target;
		}
		return "";                                                // mailto:, javascript:, ftp:, tel: ...
	}

	// ---- relative references: build them from the page's own address ----
	bool default_port = (base.scheme == "https" && base.port == "443") ||
	                    (base.scheme == "http" && base.port == "80");
	string origin = base.scheme + "://" + base.host + (default_port ? "" : ":" + base.port);

	if (href.rfind("//", 0) == 0) return base.scheme + ":" + href;       // protocol-relative
	if (href[0] == '/') return origin + remove_dots(href);               // root-relative
	string path = base.target.substr(0, base.target.find('?'));
	if (href[0] == '?') return origin + path + href;                     // query-only
	string dir = path.substr(0, path.rfind('/') + 1);               // folder of current page
	return origin + remove_dots(dir + href);                             // "page2.html", "../x"
}

// HTTP(S) download with Boost.Beast

// NOTE ON TIMEOUTS: Beast's tcp_stream::expires_after() only works with ASYNC
// operations (and setsockopt(SO_RCVTIMEO) does not work with Asio's blocking calls --
// Asio just waits again forever). So we use the async calls and run the io_context
// until each one finishes. run_op() hides that so the code below reads top-to-bottom.

// Start one async operation, wait for it to finish, throw if it failed (or timed out).
template <class StartFn>
void run_op(net::io_context& ioc, StartFn start) {
	beast::error_code ec;
	ioc.restart();
	start([&ec](beast::error_code e, auto&&...) {
		ec = e;
	});
	ioc.run();
	if (ec) throw beast::system_error(ec);
}

// One shared TLS context (loading the system CA list is slow, so do it only once)
ssl::context& tls_context() {
	static ssl::context ctx = [] {
		ssl::context c(ssl::context::tls_client);
		c.set_default_verify_paths();                 // trust the OS certificate store
		c.set_verify_mode(ssl::verify_peer);          // reject bad/expired/self-signed certs
		return c;
	}();
	return ctx;
}

// Send the request and read the full response. Works for plain and TLS streams.
template <class Stream>
void send_and_receive(net::io_context& ioc, Stream& s, http::request<http::empty_body>& req,
              http::response_parser<http::string_body>& parser) {
	beast::flat_buffer buf;
	auto& low = beast::get_lowest_layer(s);

	low.expires_after(TIMEOUT);
	run_op(ioc, [&](auto h) {
		http::async_write(s, req, h);
	});

	low.expires_after(TIMEOUT);                      // covers receiving the whole page
	beast::error_code rec;
	ioc.restart();
	http::async_read(s, buf, parser, [&](beast::error_code e, size_t) {
		rec = e;
	});
	ioc.run();
	// Servers that send "Connection: close" without a Content-Length mark the end of the
	// page by closing the socket. Over TLS that shows up as "stream truncated" even though
	// we got everything, so tell the parser "that's the end" and accept it if it's complete.
	if (rec == net::ssl::error::stream_truncated) {
		beast::error_code e2;
		parser.put_eof(e2);
		rec = e2;
	}
	if (rec) throw beast::system_error(rec);
}

// One HTTP(S) GET, NO redirect handling (get_links does that). Throws on network errors.
http::response<http::string_body> fetch(const Url& u) {
	net::io_context ioc;
	tcp::resolver resolver(ioc);
	auto const results = resolver.resolve(u.host, u.port);   // DNS (uses the OS resolver's own timeout)

	http::request<http::empty_body> req{http::verb::get, u.target, 11};
	req.set(http::field::host, u.host);
	req.set(http::field::user_agent, USER_AGENT);
	req.set(http::field::accept, "text/html");
	req.set(http::field::connection, "close");       // one request per connection: simple and safe
	// (we don't send Accept-Encoding, so servers reply uncompressed and we can parse the text)

	http::response_parser<http::string_body> parser;
	parser.body_limit(MAX_BODY);

	if (u.scheme == "https") {
		beast::ssl_stream<beast::tcp_stream> stream(ioc, tls_context());
		// SNI: tell the server which hostname we want (needed by most modern hosts)
		if (!SSL_set_tlsext_host_name(stream.native_handle(), u.host.c_str()))
			throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
			                          net::error::get_ssl_category()));
		// make sure the certificate really belongs to this host
		stream.set_verify_callback(ssl::host_name_verification(u.host));

		auto& low = beast::get_lowest_layer(stream);
		low.expires_after(TIMEOUT);
		run_op(ioc, [&](auto h) {
			low.async_connect(results, h);
		});
		low.expires_after(TIMEOUT);
		run_op(ioc, [&](auto h) {
			stream.async_handshake(ssl::stream_base::client, h);
		});
		send_and_receive(ioc, stream, req, parser);
		// No graceful TLS shutdown: a blocking shutdown can hang on a bad server,
		// and since we are done with this connection, just closing the socket is fine.
	} else {
		beast::tcp_stream stream(ioc);
		stream.expires_after(TIMEOUT);
		run_op(ioc, [&](auto h) {
			stream.async_connect(results, h);
		});
		send_and_receive(ioc, stream, req, parser);
	}
	return parser.release();
}

// 4. Finding the links in the HTML

// Uses readlib's readline(stream, '<') to split the page into chunks, one per tag.
// Each chunk is everything AFTER a '<' up to the next '<', e.g.:
//     a href="/about">About us
// We keep chunks that start with "a" + whitespace (i.e. <a ...>), cut at the first '>'
// to get just the attributes, and then look for  href = "value"  inside.
vector<string> extract_links(const string& html, const Url& base) {
	istringstream page(html);
	vector<string> out;
	unordered_set<string> seen;                          // for de-duplication

	while (true) {
		string chunk = readline(page, '<');
		if (page.fail()) break;                                    // out of input
		if (chunk.size() < 2 || tolower((unsigned char)chunk[0]) != 'a' ||
		        !isspace((unsigned char)chunk[1])) continue;      // not an <a ...> tag

		string tag = chunk.substr(0, chunk.find('>'));        // just the tag's attributes
		string low = lower(tag);

		size_t h = 0;
		while ((h = low.find("href", h)) != string::npos) {
			// must be a whole attribute name ("href", not "data-href")
			if (h == 0 || !isspace((unsigned char)low[h - 1])) {
				h += 4;
				continue;
			}
			size_t i = h + 4;
			while (i < tag.size() && isspace((unsigned char)tag[i])) ++i;
			if (i >= tag.size() || tag[i] != '=') {
				h += 4;
				continue;
			}
			++i;
			while (i < tag.size() && isspace((unsigned char)tag[i])) ++i;
			if (i >= tag.size()) break;

			string val;
			if (tag[i] == '"' || tag[i] == '\'') {                 // href="..." or href='...'
				size_t e = tag.find(tag[i], i + 1);
				if (e == string::npos) break;
				val = tag.substr(i + 1, e - i - 1);
			} else {                                               // href=unquoted
				size_t e = i;
				while (e < tag.size() && !isspace((unsigned char)tag[e])) ++e;
				val = tag.substr(i, e - i);
			}
			string absolute = resolve(base, val);
			if (!absolute.empty() && seen.insert(absolute).second) out.push_back(absolute);
			break;                                                 // one href per tag
		}
	}
	return out;
}

} // namespace


// 5. The public function

vector<string> get_links(const string& start_url) {
	string url = start_url;
	try {
		for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
			Url u;
			if (!parse_url(url, u)) {
				cerr << "[get_links] not a valid http(s) URL: " << url << "\n";
				return {};
			}
			auto res = fetch(u);
			unsigned code = res.result_int();

			if (code >= 300 && code < 400) {                 // 301/302/307/308...: go where Location says
				auto it = res.find(http::field::location);
				if (it == res.end()) {
					cerr << "[get_links] HTTP " << code << " redirect with no Location: " << url << "\n";
					return {};
				}
				url = resolve(u, string(it->value()));  // Location may be relative
				if (url.empty()) {
					cerr << "[get_links] unusable redirect target from HTTP " << code << "\n";
					return {};
				}
				continue;                                    // loop again with the new URL
			}
			if (code != 200) {
				cerr << "[get_links] HTTP " << code << " for " << url << "\n";
				return {};
			}
			auto ct = res.find(http::field::content_type);
			if (ct != res.end() && lower(string(ct->value())).find("html") == string::npos) {
				cerr << "[get_links] skipping non-HTML (" << string(ct->value()) << "): " << url << "\n";
				return {};                                   // not a web page (pdf, image, ...)
			}
			auto links = extract_links(res.body(), u);       // u = FINAL url, so relative links resolve right
			if (links.empty())
				cerr << "[get_links] HTTP 200 but no links found (" << res.body().size()
				     << " bytes) at " << url << "\n";
			return links;
		}
		cerr << "[get_links] too many redirects: " << start_url << "\n";
	} catch (const exception& e) {
		cerr << "[get_links] " << start_url << ": " << e.what() << "\n";
	}
	return {};
}
