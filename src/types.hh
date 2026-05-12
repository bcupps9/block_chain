#pragma once

#include <cstdint>

using NodeId      = std::uint32_t;
using BlockHash   = std::uint64_t;
using BlockHeight = std::uint32_t;
using Time        = double;
using EventId     = std::uint64_t;

inline constexpr BlockHash GENESIS_HASH = 0;
inline constexpr NodeId    NO_MINER     = static_cast<NodeId>(-1);
