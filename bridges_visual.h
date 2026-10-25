#pragma once
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include "crawler_graph.h"
#include "Bridges.h"
#include "GraphAdjList.h"
using namespace std;
#if __has_include("bridges_login.h")
#endif
inline const char* bridges_user() {
#ifdef BRIDGES_USER_NAME
#else
    return std::getenv("BRIDGES_USER");
#endif
}
inline const char* bridges_key() {
#ifdef BRIDGES_API_KEY
#else
    return std::getenv("BRIDGES_KEY");
#endif
}

inline const char* depth_color(int depth) {
	static const char* rainbow[] = {"red", "orange", "yellow", "green", "blue", "indigo", "violet"};
	if (depth < 0) return "gray";
	if (depth > 6) depth = 6;
	return rainbow[depth];
}

inline bool have_bridges_login() {
	return bridges_user() && bridges_key();           // inside have_bridges_login()
	const char* user = bridges_user();                // inside visualize_graph()
	const char* key = bridges_key();
}

inline bool visualize_graph(const Graph& g, const std::string& title, unsigned int assignment) {
	if (!have_bridges_login()) {
		cerr << "Set BRIDGES_USER and BRIDGES_KEY first (see the top of bridges_visual.h)\n";
		return false;
	}
	const char* user = getenv("BRIDGES_USER");
	const char* key = getenv("BRIDGES_KEY");
	try {
		bridges::Bridges viz(assignment, user, key);
		viz.setTitle(title);
		viz.setDescription("Web crawl: node color is the number of hops from the start page; "
				           "thick links point back toward the start.");
		bridges::datastructure::GraphAdjList<string, string> graph;
		for (const Node& n : g.nodes) graph.addVertex(n.url, n.url);
		for (const Edge& e : g.edges) graph.addEdge(g.nodes[e.from].url, g.nodes[e.to].url);
		for (const Node& n : g.nodes) {
			graph.getVertex(n.url)->setLabel(n.url);
			graph.getVisualizer(n.url)->setColor(depth_color(n.depth));
			graph.getVisualizer(n.url)->setSize(n.depth == 0 ? 25.0 : 10.0);
		}
		for (const Edge& e : g.edges) {
			if (!e.back) continue;
			auto* link = graph.getLinkVisualizer(g.nodes[e.from].url,g.nodes[e.to].url);
			link->setThickness(5.0);
			link->setColor("black");
		}
		graph.forceSmallVisualization(true);
		viz.setDataStructure(graph);
		viz.visualize();
		return true;
	} 
	catch (const std::exception& ex) {
		cerr << "BRIDGES error: " << ex.what()<< "\n";
	}
	catch (const std::string& msg) {
		cerr << "BRIDGES error: " << msg << "\n";
	}
	catch (...) {
		cerr << "BRIDGES error (is the API key right, and is the server reachable?)\n";
	}
	return false;
}
