// crawler_graph.h  --  Person 2: graph building + BFS/DFS traversal
#pragma once
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>   // usleep

// ---- Person 1's function (match the name/signature they actually write) ----
std::vector<std::string> get_links(const std::string& url);

// ---- Data handed to Person 3 (BRIDGES) ----
struct Edge {
    int from, to;
    bool back;          // true if the link goes to a node no farther from the root than its source
};

struct Node {
    std::string url;
    int depth = 0;      // hops from root (shortest path, see recompute_distances)
    bool fetched = false;
};

struct Graph {
    std::vector<Node> nodes;                       // nodes[0] is the root
    std::vector<Edge> edges;
    std::unordered_map<std::string, int> id;       // url -> index into nodes
};

class Crawler {
public:
    Crawler(int maxDepth = 3, size_t maxNodes = 1000, useconds_t delayUs = 100000)
        : maxDepth(maxDepth), maxNodes(maxNodes), delayUs(delayUs) {}

    Graph bfs(const std::string& start) {
        Graph g; Reset(g, start);
        std::queue<int> q; q.push(0);
        while (!q.empty()) {
            int u = q.front(); q.pop();
            if (g.nodes[u].depth >= maxDepth) continue;      // keep node, don't expand it
            for (const auto& link : Fetch(g, u)) {
                size_t before = g.nodes.size();
                int v = Link(g, u, link);
                if (v >= 0 && g.nodes.size() > before) q.push(v);   // newly discovered
            }
        }
        Finish(g);
        return g;
    }

    Graph dfs(const std::string& start) {
        Graph g; Reset(g, start);
        Visit(g, 0);
        Finish(g);
        return g;
    }

private:
    int maxDepth;
    size_t maxNodes;
    useconds_t delayUs;
    std::unordered_set<long long> seenEdges;

    void Reset(Graph& g, const std::string& start) {
        seenEdges.clear();
        std::string s = Normalize(start);
        g.nodes.push_back({s, 0, false});
        g.id[s] = 0;
    }

    void Visit(Graph& g, int u) {   // recursion depth <= maxDepth, so this is safe
        if (g.nodes[u].depth >= maxDepth) return;
        for (const auto& link : Fetch(g, u)) {
            size_t before = g.nodes.size();
            int v = Link(g, u, link);
            if (v < 0) continue;
            if (g.nodes.size() > before || !g.nodes[v].fetched) {
                if (!g.nodes[v].fetched) Visit(g, v);
            }
        }
    }

    std::vector<std::string> Fetch(Graph& g, int u) {
        g.nodes[u].fetched = true;
        usleep(delayUs);                       // be nice to the server
        return get_links(g.nodes[u].url);
    }

    // Add edge u->link (creating the node if allowed). Returns node index or -1.
    int Link(Graph& g, int u, const std::string& raw) {
        std::string url = Normalize(raw);
        if (url.empty()) return -1;
        int v;
        auto it = g.id.find(url);
        if (it != g.id.end()) v = it->second;
        else {
            if (g.nodes.size() >= maxNodes) return -1;       // graph size cap
            v = (int)g.nodes.size();
            g.nodes.push_back({url, g.nodes[u].depth + 1, false});
            g.id[url] = v;
        }
        long long key = (long long)u * 1000003LL + v;
        if (seenEdges.insert(key).second) g.edges.push_back({u, v, false});
        return v;
    }

    // DFS discovers nodes along long paths, so recompute true hop distance from
    // the root with a BFS over the finished graph, then flag back edges.
    void Finish(Graph& g) {
        std::vector<std::vector<int>> adj(g.nodes.size());
        for (auto& e : g.edges) adj[e.from].push_back(e.to);
        std::vector<int> dist(g.nodes.size(), -1);
        std::queue<int> q; q.push(0); dist[0] = 0;
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int v : adj[u]) if (dist[v] < 0) { dist[v] = dist[u] + 1; q.push(v); }
        }
        for (size_t i = 0; i < g.nodes.size(); i++) g.nodes[i].depth = dist[i];
        for (auto& e : g.edges) e.back = g.nodes[e.to].depth <= g.nodes[e.from].depth;
    }

    static std::string Normalize(std::string u) {
        auto h = u.find('#');                       // drop fragment
        if (h != std::string::npos) u.erase(h);
        while (!u.empty() && u.back() == '/') u.pop_back();
        if (u.rfind("http", 0) != 0) return "";     // skip mailto:, javascript:, relative junk
        return u;
    }
};
