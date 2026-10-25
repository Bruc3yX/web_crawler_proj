#pragma once
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include "crawler_graph.h"
#include "Bridges.h"
#include "GraphAdjList.h"

inline const char* depth_color(int depth) {
	static const char* raindbow[] = {"red", "orange", "yellow", "green", "blue", "indigo", "violet"};
	if (depth < 0) return "gray";
	if (depth > 6) depth = 6;
	return rainbow[depth];
}

inline bool have_bridges_login() {
	return std::getenv("BRIDGES_USER") && std::getenv("BRIDGES_KEY");
}

inline bool visualize_graph(const Graph& g, const std::string& title, unsigned int assignment) {
	if (!have_bridges_login()) {
		std::cerr << "Set BRIDGES_USER and BRIDGES_KEY first (see the top of bridges_visual.h)\n";
		return false;
	}
	const char* user = std::getenv("BRIDGES_USER");
	const char* key = std::getenv("BRIDGES_KEY");
	try {
		bridges::Bridges viz(assignment, user, key);
		viz.setTitle(title);
		viz.setDescription("Web crawl: node color is the number of hops from the start page; "
				           "thick links point back toward the start.");
		bridges::datastructure::GraphAdjList<std::string, std::string> graph;
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
		std::cerr << "BRIDGES error: " << ex.what()<< "\n";
	}
	catch (const std::string& msg) {
		std::cerr << "BRIDGES error: " << msg << "\n";
	}
	catch (...) {
		std::cerr << "BRIDGES error (is the API key right, and is the server reachable?)\n";
	}
	return false;
}
