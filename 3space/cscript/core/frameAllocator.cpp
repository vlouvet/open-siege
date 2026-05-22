//-----------------------------------------------------------------------------
// Copyright (C) 2024 tgemit contributors.
// See AUTHORS file and git repository for contributor information.
//
// SPDX-License-Identifier: MIT
//-----------------------------------------------------------------------------

#include "core/frameAllocator.h"

// Explicit instantiation: forces GCC to emit an out-of-line destructor
// for the thread_local smFrameAllocator (GCC 15 requires this for
// non-trivial thread_local template types).
template class ManagedAlignedBufferAllocator<U32>;

thread_local ManagedAlignedBufferAllocator<U32> FrameAllocator::smFrameAllocator;

#ifdef TORQUE_MEM_DEBUG
thread_local dsize_t FrameAllocator::smMaxAllocationBytes = 0;
#endif

