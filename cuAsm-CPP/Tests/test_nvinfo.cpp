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
#include "utils/TestUtilsCommon.hpp"

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
            if (!hasEdge(g, prev, entry.name, nameToVertex)) {
                Vertex vFrom = getOrAddVertex(g, nameToVertex, prev);
                Vertex vTo = getOrAddVertex(g, nameToVertex, entry.name);
                boost::add_edge(vFrom, vTo, g);
            }

            prev = entry.name;
        }
    }
}

/**
 * @brief Feeds every .cubin file found (recursively) under a directory into
 *        updateGraphByCubin(), mirroring the original build().
 * @param g Graph to update.
 * @param nameToVertex Map from node name to vertex descriptor, kept in sync with g.
 * @param fdir Directory to recursively search for .cubin files.
 * @return The number of .cubin files fed in.
 **/
int build(Graph& g, std::map<std::string, Vertex>& nameToVertex, const std::string& fdir) {
    int count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(fdir, ec)) {
        if (entry.path().extension() == ".cubin") {
            updateGraphByCubin(g, nameToVertex, entry.path().string());
            ++count;
        }
    }
    return count;
}

/**
 * @brief Exercises the .nv.info attribute-transition graph builder against the real CuTest
 *        fixture cubins. Previously the build() calls that actually populate the graph were
 *        commented out (pointed at nonexistent CubinSample/CubinFull directories), so this test
 *        wrote an empty graph every run and exercised none of CuNVInfo's decoding or the
 *        graph-building logic above at all. Now it points at real fixtures and asserts on the
 *        resulting graph's structure.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    Graph g;
    std::map<std::string, Vertex> nameToVertex;
    getOrAddVertex(g, nameToVertex, ROOT_NODE);

    const int cubinsFed = build(g, nameToVertex, std::string(CUASM_TESTDATA_DIR) + "/CuTest");
    t.check("at least one real .cubin was found and fed into the graph builder", cubinsFed > 0);
    t.check("the graph has more than just the Root node after feeding real cubins", boost::num_vertices(g) > 1);

    const auto rootV = nameToVertex.at(ROOT_NODE);
    t.check("the Root node has at least one outgoing edge (every kernel's first .nv.info "
            "attribute)",
            boost::out_degree(rootV, g) > 0);

    // Every one of these is a common EIATTR that essentially every real kernel's .nv.info carries
    // (frame/stack size accounting, param bank size); their presence confirms CuNVInfo actually
    // decoded real attribute data, not just empty/garbage sections.
    // Per-kernel (.nv.info.<kernel>) attributes every one of CuTest's kernels carries: its
    // parameter bank location/size and per-parameter layout info, plus (since every kernel here
    // has an EXIT instruction and sm_61 doesn't auto-generate that attribute away) its exit
    // instruction offsets. Confirms CuNVInfo actually decoded real per-kernel attribute data, not
    // just empty/garbage sections.
    t.check("well-known, near-universal per-kernel EIATTR names show up as graph nodes",
            nameToVertex.count("EIATTR_CBANK_PARAM_SIZE") && nameToVertex.count("EIATTR_PARAM_CBANK") &&
                nameToVertex.count("EIATTR_KPARAM_INFO") && nameToVertex.count("EIATTR_EXIT_INSTR_OFFSETS"));

    bool anyUnknown = false;
    for (const auto& [name, v] : nameToVertex) {
        (void)v;
        if (name.starts_with("EIATTR_UNKNOWN_")) {
            anyUnknown = true;
        }
    }
    t.check("none of these real, nvcc-produced fixtures contain an attribute CuNVInfo doesn't recognize",
            !anyUnknown);

    // Best-effort: render the graph via graphviz, matching the original script's intent. Not part
    // of the pass/fail contract, since it depends on `dot` being installed and isn't what this
    // test is actually verifying.
    try {
        const std::string dotName = std::string(CUASM_TESTDATA_DIR) + "/CuTest/tmp_nvinfo_graph.dot";
        const std::string pngName = std::string(CUASM_TESTDATA_DIR) + "/CuTest/tmp_nvinfo_graph.png";
        std::ofstream dotFile(dotName);
        boost::write_graphviz(dotFile, g, boost::make_label_writer(boost::get(boost::vertex_name, g)));
        dotFile.close();
        CuAsm::checkOutput({"dot", "-Tpng", dotName, "-o", pngName});
        std::error_code ec;
        fs::remove(dotName, ec);
        fs::remove(pngName, ec);
    } catch (const std::exception& e) {
        std::cout << "[INFO] graphviz rendering skipped (non-fatal): " << e.what() << "\n";
    }

    return t.finish("test_nvinfo");
}
