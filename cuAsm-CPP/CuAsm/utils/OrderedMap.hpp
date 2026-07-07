#pragma once

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace CuAsm {

/**
 * @brief A map that iterates in insertion order, mirroring python's insertion-ordered `dict`.
 *        Entries are kept in a vector (for order-preserving iteration) alongside a hash index (for
 *        O(1) lookup); overwriting an existing key updates its value in place without moving it,
 *        matching `dict.__setitem__` semantics.
 * @tparam Key Key type; must be hashable via std::hash.
 * @tparam Value Mapped value type.
 **/
template <typename Key, typename Value>
class OrderedMap {
public:
    using Entry = std::pair<Key, Value>;
    using iterator = typename std::vector<Entry>::iterator;
    using const_iterator = typename std::vector<Entry>::const_iterator;

    /** @brief Gets an iterator to the first entry in insertion order. */
    iterator begin() { return m_Entries.begin(); }
    /** @brief Gets the end iterator. */
    iterator end() { return m_Entries.end(); }
    /** @brief Const overload of begin(). */
    const_iterator begin() const { return m_Entries.begin(); }
    /** @brief Const overload of end(). */
    const_iterator end() const { return m_Entries.end(); }

    /**
     * @brief Looks up an entry by key, mirroring dict lookup.
     * @param key Key to search for.
     * @return Iterator to the entry, or end() if absent.
     **/
    iterator find(const Key& key) {
        const auto it = m_Index.find(key);
        return it == m_Index.end() ? m_Entries.end() : m_Entries.begin() + static_cast<std::ptrdiff_t>(it->second);
    }

    /** @brief Const overload of find(). */
    const_iterator find(const Key& key) const {
        const auto it = m_Index.find(key);
        return it == m_Index.end() ? m_Entries.end() : m_Entries.begin() + static_cast<std::ptrdiff_t>(it->second);
    }

    /**
     * @brief Looks up an entry's value by key, mirroring `__getitem__`.
     * @param key Key to search for.
     * @return Reference to the value.
     * @throws std::out_of_range if key is absent.
     **/
    Value& at(const Key& key) {
        const auto it = find(key);
        if (it == end()) {
            throw std::out_of_range("OrderedMap::at: key not found");
        }
        return it->second;
    }

    /** @brief Const overload of at(). */
    const Value& at(const Key& key) const {
        const auto it = find(key);
        if (it == end()) {
            throw std::out_of_range("OrderedMap::at: key not found");
        }
        return it->second;
    }

    /**
     * @brief Inserts a new entry (appended at the end) or overwrites an existing one in place,
     *        mirroring `__setitem__`.
     * @param key Key to insert/overwrite.
     * @param value New value.
     **/
    void insert_or_assign(const Key& key, Value value) {
        const auto idxIt = m_Index.find(key);
        if (idxIt != m_Index.end()) {
            m_Entries[idxIt->second].second = std::move(value);
        } else {
            m_Index.emplace(key, m_Entries.size());
            m_Entries.emplace_back(key, std::move(value));
        }
    }

    /**
     * @brief Inserts a value constructed in place if key is absent, mirroring dict's
     *        check-then-insert pattern (`if key not in d: d[key] = ...`); a no-op if key is
     *        already present, mirroring std::map::emplace.
     * @param key Key to insert under.
     * @param args Constructor arguments for the mapped value.
     * @return Iterator to the (possibly pre-existing) entry, and whether an insertion happened.
     **/
    template <typename... Args>
    std::pair<iterator, bool> emplace(const Key& key, Args&&... args) {
        const auto idxIt = m_Index.find(key);
        if (idxIt != m_Index.end()) {
            return {m_Entries.begin() + static_cast<std::ptrdiff_t>(idxIt->second), false};
        }
        m_Index.emplace(key, m_Entries.size());
        m_Entries.emplace_back(key, Value(std::forward<Args>(args)...));
        return {std::prev(m_Entries.end()), true};
    }

    /**
     * @brief Removes an entry by key, mirroring `__delitem__`.
     * @param key Key to remove.
     * @return 1 if an entry was removed, 0 if key was absent.
     **/
    std::size_t erase(const Key& key) {
        const auto idxIt = m_Index.find(key);
        if (idxIt == m_Index.end()) {
            return 0;
        }
        const std::size_t pos = idxIt->second;
        m_Entries.erase(m_Entries.begin() + static_cast<std::ptrdiff_t>(pos));
        m_Index.erase(idxIt);
        for (auto& [k, i] : m_Index) {
            (void)k;
            if (i > pos) {
                --i;
            }
        }
        return 1;
    }

    /** @brief Number of entries, mirroring `__len__`. */
    std::size_t size() const { return m_Entries.size(); }

    /** @brief Whether the map has no entries. */
    bool empty() const { return m_Entries.empty(); }

    /** @brief Removes every entry. */
    void clear() {
        m_Entries.clear();
        m_Index.clear();
    }

private:
    std::vector<Entry> m_Entries;
    std::unordered_map<Key, std::size_t> m_Index;
};

} // namespace CuAsm
