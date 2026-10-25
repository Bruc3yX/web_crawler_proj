#include <cstdlib>
#include <iostream>
#include <string>
#include "crawler_graph.h"
#include "bridges_visual.h"
using namespace std;

int main(int argc, char** argv) {
	if (argc < 3) {
		cerr << "usage: " << argv[0]
							   << " <bfs|dfs> <start url> [max depth = 3] 9max nodes = 1000] [bridges assignment = 1]\n";
		return 1;
	}
	string mode = argv[1];
	string start = argv[2];
	int maxDepth = (argc > 3) ? atoi(argv[3]) : 3;
	size_t maxNodes = (argc > 4) ? atoi(argv[4]) : 1000;
	unsigned int assignment = (argc > 5) ? atoi(argv[5]) : 1;
	if (mode != "bfs" && mode != "dfs") {
		cerr << "first argument must be bfs or dfs\n";
		return 1;
	}
	if (!have_bridges_login()) {
		cerr << "Set BRIDGES_USER and BRIDGES_KEY first (see the top of bridges_visual.h)\n";
		return 1;
	}
	Crawler crawler(maxDepth, maxNodes);
	cerr << "crawling " << start << " (" << mode << ", depth " << maxDepth << ", up to " << maxNodes << " pages)...\n";
	Graph g = (mode == "bfs") ? crawler.bfs(start) : crawler.dfs(start);
	size_t back = 0;
	for (const Edge& e : g.edges) if (e.back) back++;
	cout << g.nodes.size() << " pages, " << g.edges.size()<< " links (" << back << " point back)\n";
	string title = "Crawl of " + start + " (" + mode + ", depth " + to_string(maxDepth) + ")";
	return visualize_graph(g, title, assignment) ? 0 : 1;
}
