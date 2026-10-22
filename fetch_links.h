// fetch_links.h  --  Person 1: fetch a page over HTTP/HTTPS and return the URLs linked from it
#pragma once
#include <string>
#include <vector>

// Returns absolute, de-duplicated http(s) URLs found in <a href="..."> tags.
// Follows up to 5 redirects. Returns an empty vector on any failure.
std::vector<std::string> get_links(const std::string& url);
