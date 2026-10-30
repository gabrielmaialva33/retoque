// Stand-in for ankerl::unordered_dense on hosts without external/unordered_dense (host CI checks out only
// simpleini, spdlog and vulkan). The tracking code needs nothing beyond the associative-map interface.
#pragma once

#include <functional>
#include <unordered_map>

namespace ankerl::unordered_dense
{
template <class Key, class T, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
using map = std::unordered_map<Key, T, Hash, KeyEqual>;
} // namespace ankerl::unordered_dense
