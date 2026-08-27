// Heap-allocation guard: operator new aborts deterministically while armed.
#pragma once

namespace mog::alloc {

// Arms/disarms the allocation tripwire; returns the previous state.
bool arm() noexcept;
bool disarm() noexcept;
bool is_armed() noexcept;

} // namespace mog::alloc
