// Copyright (c) 2014-2016 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PYSTON_CORE_ORDEREDDENSEMAP_H
#define PYSTON_CORE_ORDEREDDENSEMAP_H

#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

#include "core/common.h"
#include "core/from_llvm/DenseMap.h"

namespace pyston {

// A hash map that iterates in insertion order, like CPython's compact dict: the entries live
// in an array in the order they were added, and a DenseMap maps each key to its position.
// Erased entries become holes (their key is set to the tombstone key) until the array is
// compacted, which only happens when it is full.
//
// It mirrors the parts of the DenseMap interface that BoxedDict uses. Iterators are positions
// in the entry array, so they stay valid when entries are added.
template <typename KeyT, typename ValueT, typename KeyInfoT> class OrderedDenseMap {
public:
    struct value_type {
        KeyT first;
        ValueT second;
    };
    static_assert(std::is_trivially_copyable<value_type>::value, "entries are moved with realloc");

private:
    typedef DenseMap<KeyT, uint32_t, KeyInfoT, detail::DenseMapPair<KeyT, uint32_t>, /* MinSize= */ 8> IndexMap;

    IndexMap index;
    value_type* entries = nullptr;
    uint32_t num_used = 0; // entries in use, including holes
    uint32_t capacity = 0;
    uint32_t num_live = 0;

    bool isHole(uint32_t pos) const { return KeyInfoT::isEqual(entries[pos].first, KeyInfoT::getTombstoneKey()); }

    uint32_t skipHoles(uint32_t pos) const {
        while (pos < num_used && isHole(pos))
            pos++;
        return pos;
    }

    void reserveEntries(uint32_t n) {
        if (n <= capacity)
            return;
        uint32_t new_capacity = capacity ? capacity : 8;
        while (new_capacity < n)
            new_capacity *= 2;
        entries = (value_type*)realloc(entries, sizeof(value_type) * new_capacity);
        RELEASE_ASSERT(entries, "out of memory");
        capacity = new_capacity;
    }

    // Removes the holes, renumbering the positions stored in the index.
    void compact() {
        uint32_t* new_pos = (uint32_t*)malloc(sizeof(uint32_t) * num_used);
        uint32_t n = 0;
        for (uint32_t i = 0; i < num_used; i++) {
            if (isHole(i))
                continue;
            new_pos[i] = n;
            entries[n++] = entries[i];
        }
        for (auto&& p : index)
            p.second = new_pos[p.second];
        free(new_pos);
        num_used = n;
    }

public:
    class iterator {
    private:
        OrderedDenseMap* map;
        uint32_t pos;

        bool atEnd() const { return !map || pos >= map->num_used; }

    public:
        iterator() : map(nullptr), pos(0) {}
        iterator(OrderedDenseMap* map, uint32_t pos) : map(map), pos(map->skipHoles(pos)) {}

        bool operator==(const iterator& rhs) const {
            if (atEnd() || rhs.atEnd())
                return atEnd() && rhs.atEnd();
            return pos == rhs.pos;
        }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }
        iterator& operator++() {
            pos = map->skipHoles(pos + 1);
            return *this;
        }
        value_type& operator*() const { return map->entries[pos]; }
        value_type* operator->() const { return &map->entries[pos]; }

        // The position in the entry array, which is what PyDict_Next stores in its cursor.
        uint32_t position() const { return pos; }
    };
    typedef iterator const_iterator;

    OrderedDenseMap() {}
    OrderedDenseMap(const OrderedDenseMap& other) { copyFrom(other); }
    OrderedDenseMap& operator=(const OrderedDenseMap&) = delete;
    ~OrderedDenseMap() { free(entries); }

    uint32_t size() const { return num_live; }
    bool empty() const { return num_live == 0; }

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, UINT32_MAX); }
    iterator begin() const { return const_cast<OrderedDenseMap*>(this)->begin(); }
    iterator end() const { return const_cast<OrderedDenseMap*>(this)->end(); }

    // Iterator to the first live entry at or after `pos`.
    iterator fromPosition(uint32_t pos) { return iterator(this, pos); }

    // Iterator to the most recently added entry, or end() if there is none.
    iterator last() {
        for (uint32_t pos = num_used; pos > 0; pos--) {
            if (!isHole(pos - 1))
                return iterator(this, pos - 1);
        }
        return end();
    }

    iterator find(const KeyT& key) {
        auto it = index.find(key);
        if (it == index.end())
            return end();
        return iterator(this, it->second);
    }

    size_t count(const KeyT& key) const { return index.count(key); }

    // The value for `key`, or a default-constructed value if it isn't in the map.
    ValueT lookup(const KeyT& key) const {
        auto it = index.find(key);
        if (it == index.end())
            return ValueT();
        return entries[it->second].second;
    }

    std::pair<iterator, bool> insert(const std::pair<KeyT, ValueT>& kv) {
        if (num_used == capacity && num_live < num_used / 2)
            compact();
        reserveEntries(num_used + 1);

        auto r = index.insert(std::make_pair(kv.first, num_used));
        if (!r.second)
            return std::make_pair(iterator(this, r.first->second), false);
        entries[num_used].first = kv.first;
        entries[num_used].second = kv.second;
        num_used++;
        num_live++;
        return std::make_pair(iterator(this, num_used - 1), true);
    }

    ValueT& operator[](const KeyT& key) { return insert(std::make_pair(key, ValueT())).first->second; }

    void erase(iterator it) {
        uint32_t pos = it.position();
        index.erase(entries[pos].first);
        entries[pos].first = KeyInfoT::getTombstoneKey();
        entries[pos].second = ValueT();
        num_live--;
        if (num_live == 0)
            num_used = 0;
    }

    bool erase(const KeyT& key) {
        auto it = find(key);
        if (it == end())
            return false;
        erase(it);
        return true;
    }

    // Makes this map a copy of `other`, without hashing or comparing any keys.
    void copyFrom(const OrderedDenseMap& other) {
        clear();
        index = other.index;
        reserveEntries(other.num_used);
        if (other.num_used)
            memcpy(entries, other.entries, sizeof(value_type) * other.num_used);
        num_used = other.num_used;
        num_live = other.num_live;
    }

    void swap(OrderedDenseMap& other) {
        index.swap(other.index);
        std::swap(entries, other.entries);
        std::swap(num_used, other.num_used);
        std::swap(capacity, other.capacity);
        std::swap(num_live, other.num_live);
    }

    void freeAllMemory() { clear(); }

    void clear() {
        index.clear();
        free(entries);
        entries = nullptr;
        num_used = capacity = num_live = 0;
    }

    void grow(uint32_t n) {
        index.grow(n);
        reserveEntries(n);
    }
};

} // namespace pyston

#endif
