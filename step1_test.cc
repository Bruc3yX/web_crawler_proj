#include "fetch_links.h"
#include "read.h"
#include <iostream>
using namespace std;

int main(int argc, char** argv) {
	string url = (argc > 1) ? argv[1] : read<string>("URL to fetch: ");
	auto links = get_links(url);
	cout << "Found " << links.size() << " links on " << url << "\n";
	for (const auto& l : links) cout << "  " << l << "\n";
}
