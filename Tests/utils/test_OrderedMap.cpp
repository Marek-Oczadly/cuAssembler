#include <stdexcept>
#include <string>

#include "../../CuAsm/utils/OrderedMap.hpp"
#include "TestUtilsCommon.hpp"

using CuAsm::OrderedMap;

namespace {

/** @brief Concatenates every key in iteration order, e.g. for comparing against an expected order. */
std::string keyOrder(const OrderedMap<std::string, int>& m) {
    std::string s;
    for (const auto& [k, v] : m) {
        (void)v;
        s += k;
    }
    return s;
}

} // namespace

/**
 * @brief Exercises CuAsm::OrderedMap, the insertion-ordered dict stand-in used throughout the
 *        parser/assembler for symbol/section/label tables where iteration order must match
 *        python dict semantics. Covers the properties those callers actually depend on:
 *        insertion order surviving overwrites, in-place erase re-indexing the hash side without
 *        breaking subsequent lookups, and emplace's check-then-insert (no-op if present) contract.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    OrderedMap<std::string, int> m;
    t.check("a freshly constructed map is empty (size 0)", m.empty() && m.size() == 0);

    m.insert_or_assign("a", 1);
    m.insert_or_assign("b", 2);
    m.insert_or_assign("c", 3);
    t.check("insert_or_assign grows the map and at() retrieves the right value", m.size() == 3 && m.at("b") == 2);

    t.check("find() on an absent key returns end(), at() on it throws std::out_of_range", m.find("z") == m.end());
    t.checkThrows<std::out_of_range>("at() on an absent key throws std::out_of_range", [&m] { (void)m.at("z"); });

    t.checkEqual("iteration order matches insertion order", keyOrder(m), std::string("abc"));

    m.insert_or_assign("a", 100); // overwrite: must update in place, not move to the end
    t.check("overwriting an existing key updates its value without moving it in iteration order",
            m.at("a") == 100 && keyOrder(m) == "abc");

    auto [itNew, insertedNew] = m.emplace("d", 4);
    auto [itOld, insertedOld] = m.emplace("a", 999);
    t.check("emplace inserts a new key but is a no-op on an existing one",
            insertedNew && itNew->second == 4 && !insertedOld && itOld->second == 100);

    t.check("erasing an absent key reports 0 removed; erasing a present key reports 1", m.erase("z") == 0 && m.erase("b") == 1);

    t.check("erasing a middle entry doesn't corrupt lookups for entries that came after it",
            m.find("b") == m.end() && m.at("c") == 3 && m.at("d") == 4 && keyOrder(m) == "acd");

    m.clear();
    t.check("clear() empties the map and subsequent at() calls throw", m.empty() && m.size() == 0);
    t.checkThrows<std::out_of_range>("at() throws on any key after clear()", [&m] { (void)m.at("a"); });

    return t.finish("test_OrderedMap");
}
