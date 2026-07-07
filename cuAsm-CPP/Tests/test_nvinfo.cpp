#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <elfio/elfio.hpp>

#include "../CuAsm/CuNVInfo.hpp"
#include "../CuAsm/common.hpp"

namespace fs = std::filesystem;

using Graph = boost::adjacency_list<boost::setS, boost::vecS, boost::directedS,
                                     boost::property<boost::vertex_name_t, std::string>>;
using Vertex = boost::graph_traits<Graph>::vertex_descriptor;

const std::string ROOT_NODE = "Root";

/**
 * @brief Looks up (adding if necessary) the vertex for a node name, mirroring how the original
 *        relies on networkx's implicit node creation in add_edge().
 * @param g Graph to look up/add the vertex in.
 * @param nameToVertex Map from node name to vertex descriptor, kept in sync with g.
 * @param name Node name to look up.
 * @return The vertex descriptor for name.
 **/
Vertex getOrAddVertex(Graph& g, std::map<std::string, Vertex>& nameToVertex, const std::string& name) {
    auto it = nameToVertex.find(name);
    if (it != nameToVertex.end()) {
        return it->second;
    }

    Vertex v = boost::add_vertex(name, g);
    nameToVertex[name] = v;
    return v;
}

/**
 * @brief Checks whether an edge already exists between two named nodes, mirroring networkx's
 *        DiGraph.has_edge().
 * @param g Graph to check.
 * @param from Source node name.
 * @param to Destination node name.
 * @param nameToVertex Map from node name to vertex descriptor.
 * @return True if the edge from -> to exists.
 **/
bool hasEdge(const Graph& g, const std::string& from, const std::string& to,
             const std::map<std::string, Vertex>& nameToVertex) {
    auto itFrom = nameToVertex.find(from);
    auto itTo = nameToVertex.find(to);
    if (itFrom == nameToVertex.end() || itTo == nameToVertex.end()) {
        return false;
    }

    return boost::edge(itFrom->second, itTo->second, g).second;
}

/**
 * @brief Decodes every .nv.info.* section of a cubin and adds an edge for each attribute
 *        transition seen, mirroring the original updateGraphByCubin().
 * @param g Graph to update.
 * @param nameToVertex Map from node name to vertex descriptor, kept in sync with g.
 * @param cubinname Path of the cubin to read.
 **/
void updateGraphByCubin(Graph& g, std::map<std::string, Vertex>& nameToVertex, const std::string& cubinname) {
    ELFIO::elfio ef;
    if (!ef.load(cubinname)) {
        std::cerr << "Failed to load " << cubinname << std::endl;
        return;
    }

    for (const auto& sec : ef.sections) {
        if (sec->get_name().rfind(".nv.info.", 0) != 0) {
            continue;
        }

        const char* data = sec->get_data();
        std::vector<std::uint8_t> bytecodes(data, data + sec->get_size());
        CuAsm::CuNVInfo nvinfo(bytecodes);

        std::string prev = ROOT_NODE;
        for (const auto& entry : nvinfo.m_AttrList) {
            if (entry.name.rfind("EIATTR_UNKNOWN_", 0) == 0) {
                std::cout << cubinname << std::endl;
            }

            if (!hasEdge(g, prev, entry.name, nameToVertex)) {
                Vertex vFrom = getOrAddVertex(g, nameToVertex, prev);
                Vertex vTo = getOrAddVertex(g, nameToVertex, entry.name);
                boost::add_edge(vFrom, vTo, g);
                std::cout << "Add edge " << prev << " to " << entry.name << std::endl;
            }

            prev = entry.name;
        }
    }
}

/**
 * @brief Feeds every .cubin file found (recursively) under a directory pattern into
 *        updateGraphByCubin(), mirroring the original build().
 * @param g Graph to update.
 * @param nameToVertex Map from node name to vertex descriptor, kept in sync with g.
 * @param fdir Directory to recursively search for .cubin files.
 **/
void build(Graph& g, std::map<std::string, Vertex>& nameToVertex, const std::string& fdir) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(fdir, ec)) {
        if (entry.path().extension() == ".cubin") {
            updateGraphByCubin(g, nameToVertex, entry.path().string());
        }
    }
}

/**
 * @brief Builds the (empty, since the original's build() calls are commented out) attribute
 *        transition graph, mirroring the original getGraph().
 * @param nameToVertex Output map from node name to vertex descriptor, kept in sync with the graph.
 * @return The constructed graph.
 **/
Graph getGraph(std::map<std::string, Vertex>& nameToVertex) {
    Graph g;
    // getOrAddVertex(g, nameToVertex, ROOT_NODE);
    // build(g, nameToVertex, std::string(CUASM_TESTDATA_DIR) + "/CubinSample");
    // build(g, nameToVertex, std::string(CUASM_TESTDATA_DIR) + "/CubinFull");

    return g;
}

/**
 * @brief Entry point mirroring the original Tests/test_nvinfo.py script: builds the attribute
 *        transition graph and renders it with Graphviz, replacing plt.show() (no portable
 *        equivalent) with saving to a PNG, as the original's own commented-out alternative did.
 * @return 0 on success.
 **/
int main() {
    std::map<std::string, Vertex> nameToVertex;
    Graph g = getGraph(nameToVertex);

    std::string dotName = "nx_test.dot";
    std::ofstream dotFile(dotName);
    boost::write_graphviz(dotFile, g, boost::make_label_writer(boost::get(boost::vertex_name, g)));
    dotFile.close();

    CuAsm::checkOutput({"dot", "-Tpng", dotName, "-o", "nx_test.png"});

    return 0;
}
