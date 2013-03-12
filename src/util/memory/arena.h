// Copyright 2010 Google Inc.  All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
// Memory arena for variable-length datatypes and STL collections.

#ifndef SUPERSONIC_BASE_MEMORY_ARENA_H_
#define SUPERSONIC_BASE_MEMORY_ARENA_H_

#include <stddef.h>
#include <string.h>

#include <memory>
#include <new>
#include <tr1/memory>

#include <boost/thread/mutex.hpp>

#include <glog/logging.h>
#include "gutil/logging-inl.h"
#include "gutil/macros.h"
#include "gutil/scoped_ptr.h"
#include "gutil/strings/stringpiece.h"
#include "util/memory/memory.h"
#include "util/slice.h"

using std::allocator;

namespace kudu {
using std::tr1::shared_ptr;

template<bool THREADSAFE> struct ArenaTraits;

template <> struct ArenaTraits<true> {
  typedef Atomic32 offset_type;
};

template <> struct ArenaTraits<false> {
  typedef uint32_t offset_type;
};

// A helper class for storing variable-length blobs (e.g. strings). Once a blob
// is added to the arena, its index stays fixed. No reallocation happens.
// Instead, the arena keeps a list of buffers. When it needs to grow, it
// allocates a new buffer. Each subsequent buffer is 2x larger, than its
// predecessor, until the maximum specified buffer size is reached.
// The buffers are furnished by a designated allocator.
//
// This class is thread-safe with the fast path lock-free.
template <bool THREADSAFE>
class ArenaBase {
 public:
  // Creates a new arena, with a single buffer of size up-to
  // initial_buffer_size, upper size limit for later-allocated buffers capped
  // at max_buffer_size, and maximum capacity (i.e. total sizes of all buffers)
  // possibly limited by the buffer allocator. The allocator might cap the
  // initial allocation request arbitrarily (down to zero). As a consequence,
  // arena construction never fails due to OOM.
  //
  // Calls to AllocateBytes() will then give out bytes from the working buffer
  // until it is exhausted. Then, a subsequent working buffer will be allocated.
  // The size of the next buffer is normally 2x the size of the previous buffer.
  // It might be capped by the allocator, or by the max_buffer_size parameter.
  ArenaBase(BufferAllocator* const buffer_allocator,
            size_t initial_buffer_size,
            size_t max_buffer_size);

  // Creates an arena using a default (heap) allocator with unbounded capacity.
  // Discretion advised.
  ArenaBase(size_t initial_buffer_size, size_t max_buffer_size);

  // Adds content of the specified Slice to the arena, and returns a
  // pointer to it. The pointer is guaranteed to remain valid during the
  // lifetime of the arena. The Slice object itself is not copied. The
  // size information is not stored.
  // (Normal use case is that the caller already has an array of Slices,
  // where it keeps these pointers together with size information).
  // If this request would make the arena grow and the allocator denies that,
  // returns NULL and leaves the arena unchanged.
  uint8_t *AddSlice(const Slice& value);

  // Same as above.
  void * AddBytes(const void *data, size_t len);

  // Handy wrapper for placement-new
  template<class T>
  T *NewObject();

  // Handy wrapper for placement-new
  template<class T, typename A1>
  T *NewObject(A1 arg1);

  // Handy wrapper for placement-new
  template<class T, typename A1, typename A2>
  T *NewObject(A1 arg1, A2 arg2);

  // Handy wrapper for placement-new
  template<class T, typename A1, typename A2, typename A3>
  T *NewObject(A1 arg1, A2 arg2, A3 arg3);

  // Relocate the given Slice into the arena, setting 'dst' and
  // returning true if successful.
  // It is legal for 'dst' to be a pointer to 'src'.
  // See AddSlice above for detail on memory lifetime.
  bool RelocateSlice(const Slice &src, Slice *dst);

  // Reserves a blob of the specified size in the arena, and returns a pointer
  // to it. The caller can then fill the allocated memory. The pointer is
  // guaranteed to remain valid during the lifetime of the arena.
  // If this request would make the arena grow and the allocator denies that,
  // returns NULL and leaves the arena unchanged.
  void* AllocateBytes(const size_t size) {
    return AllocateBytesAligned(size, 1);
  }

  // Allocate bytes, ensuring a specified alignment.
  // NOTE: alignment MUST be a power of two, or else this will break.
  void* AllocateBytesAligned(const size_t size, const size_t alignment);

  // Removes all data from the arena. (Invalidates all pointers returned by
  // AddSlice and AllocateBytes). Does not cause memory allocation.
  // May reduce memory footprint, as it discards all allocated buffers but
  // the last one.
  // Unless allocations exceed max_buffer_size, repetitive filling up and
  // resetting normally lead to quickly settling memory footprint and ceasing
  // buffer allocations, as the arena keeps reusing a single, large buffer.
  void Reset();

  // Returns the memory footprint of this arena, in bytes, defined as a sum of
  // all buffer sizes. Always greater or equal to the total number of
  // bytes allocated out of the arena.
  size_t memory_footprint() const { return arena_footprint_; }

 private:
  // Encapsulates a single buffer in the arena.
  class Component;

  // Fallback for AllocateBytes non-fast-path
  void* AllocateBytesFallback(const size_t size, const size_t align);

  Component* NewComponent(size_t requested_size, size_t minimum_size);
  void AddComponent(Component *component);

  BufferAllocator* const buffer_allocator_;
  vector<shared_ptr<Component> > arena_;
  Component* current_;
  const size_t max_buffer_size_;
  size_t arena_footprint_;

  // Lock covering 'slow path' allocation, when new components are
  // allocated and added to the arena's list. Also covers any other
  // mutation of the component data structure (eg Reset).
  boost::mutex component_lock_;

  DISALLOW_COPY_AND_ASSIGN(ArenaBase);
};

// STL-compliant allocator, for use with hash_maps and other structures
// needed by transformations. Enables memory control and improves performance.
// (The code is shamelessly stolen from base/arena-inl.h).
template<class T, bool THREADSAFE> class ArenaAllocator {
 public:
  typedef T value_type;
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;

  typedef T* pointer;
  typedef const T* const_pointer;
  typedef T& reference;
  typedef const T& const_reference;
  pointer index(reference r) const  { return &r; }
  const_pointer index(const_reference r) const  { return &r; }
  size_type max_size() const  { return size_t(-1) / sizeof(T); }

  explicit ArenaAllocator(ArenaBase<THREADSAFE>* arena) : arena_(arena) {
    CHECK_NOTNULL(arena_);
  }

  ~ArenaAllocator() { }

  pointer allocate(size_type n, allocator<void>::const_pointer /*hint*/ = 0) {
    return reinterpret_cast<T*>(arena_->AllocateBytes(n * sizeof(T)));
  }

  void deallocate(pointer p, size_type n) {}

  void construct(pointer p, const T& val) {
    new(reinterpret_cast<void*>(p)) T(val);
  }

  void destroy(pointer p) { p->~T(); }

  template<class U, bool TS> struct rebind {
    typedef ArenaAllocator<U, TS> other;
  };

  template<class U, bool TS> ArenaAllocator(const ArenaAllocator<U, TS>& other)
      : arena_(other.arena()) { }

  template<class U, bool TS> bool operator==(const ArenaAllocator<U, TS>& other) const {
    return arena_ == other.arena();
  }

  template<class U, bool TS> bool operator!=(const ArenaAllocator<U, TS>& other) const {
    return arena_ != other.arena();
  }

  ArenaBase<THREADSAFE> *arena() const {
    return arena_;
  }

 private:

  ArenaBase<THREADSAFE>* arena_;
};


class Arena : public ArenaBase<false> {
public:
  explicit Arena(size_t initial_buffer_size, size_t max_buffer_size) :
    ArenaBase<false>(initial_buffer_size, max_buffer_size)
  {}
};

class ThreadSafeArena : public ArenaBase<true> {
public:
  explicit ThreadSafeArena(size_t initial_buffer_size, size_t max_buffer_size) :
    ArenaBase<true>(initial_buffer_size, max_buffer_size)
  {}
};

// Implementation of inline and template methods

template<bool THREADSAFE>
class ArenaBase<THREADSAFE>::Component {
 public:
  explicit Component(Buffer* buffer)
      : buffer_(buffer),
        data_(static_cast<uint8_t*>(buffer->data())),
        offset_(0),
        size_(buffer->size()) {}

  // Tries to reserve space in this component. Returns the pointer to the
  // reserved space if successful; NULL on failure (if there's no more room).
  uint8_t* AllocateBytes(const size_t size) {
    return AllocateBytesAligned(size, 1);
  }

  uint8_t *AllocateBytesAligned(const size_t size, const size_t alignment);

  size_t size() const { return size_; }
  void Reset() { offset_ = 0; }

 private:
  scoped_ptr<Buffer> buffer_;
  uint8_t* const data_;
  typename ArenaTraits<THREADSAFE>::offset_type offset_;
  const size_t size_;
  DISALLOW_COPY_AND_ASSIGN(Component);
};


// Thread-safe implementation
template <>
inline uint8_t *ArenaBase<true>::Component::AllocateBytesAligned(const size_t size, const size_t alignment) {
  // Special case check the alignments that are useful
  DCHECK(alignment == 1 || alignment == 2 || alignment == 4 ||
         alignment == 8 || alignment == 16 || alignment == 64)
    << "bad alignment: " << alignment;
  retry:
  Atomic32 offset = Acquire_Load(&offset_);

  Atomic32 aligned = (offset + (alignment - 1)) & ~(alignment - 1);
  Atomic32 new_offset = aligned + size;

  if (PREDICT_TRUE(new_offset <= size_)) {
    bool success = Acquire_CompareAndSwap(&offset_, offset, new_offset) == offset;
    if (PREDICT_TRUE(success)) {
      return data_ + aligned;
    } else {
      // Raced with another allocator
      goto retry;
    }
  } else {
    return NULL;
  }
}

// Non-Threadsafe implementation
template <>
inline uint8_t *ArenaBase<false>::Component::AllocateBytesAligned(const size_t size, const size_t alignment) {
  size_t aligned = (offset_ + (alignment - 1)) & ~(alignment - 1);
  uint8_t* destination = data_ + aligned;
  size_t save_offset = offset_;
  offset_ = aligned + size;
  if (PREDICT_TRUE(offset_ <= size_)) {
    return destination;
  } else {
    offset_ = save_offset;
    return NULL;
  }
}


// Fast-path allocation should get inlined, and fall-back
// to non-inline function call for allocation failure
template <bool THREADSAFE>
inline void *ArenaBase<THREADSAFE>::AllocateBytesAligned(const size_t size, const size_t align) {
  void* result = current_->AllocateBytesAligned(size, align);
  if (PREDICT_TRUE(result != NULL)) return result;
  return AllocateBytesFallback(size, align);
}

template <bool THREADSAFE>
inline uint8_t* ArenaBase<THREADSAFE>::AddSlice(const Slice& value) {
  return reinterpret_cast<uint8_t *>(AddBytes(value.data(), value.size()));
}

template <bool THREADSAFE>
inline void *ArenaBase<THREADSAFE>::AddBytes(const void *data, size_t len) {
  void* destination = AllocateBytes(len);
  if (destination == NULL) return NULL;
  memcpy(destination, data, len);
  return destination;
}

template <bool THREADSAFE>
inline bool ArenaBase<THREADSAFE>::RelocateSlice(const Slice &src, Slice *dst) {
  void* destination = AllocateBytes(src.size());
  if (destination == NULL) return false;
  memcpy(destination, src.data(), src.size());
  *dst = Slice(reinterpret_cast<uint8_t *>(destination), src.size());
  return true;
}


template<bool THREADSAFE>
template<class T>
inline T *ArenaBase<THREADSAFE>::NewObject() {
  void *mem = AllocateBytes(sizeof(T));
  if (mem == NULL) throw std::bad_alloc();
  return new (mem) T();
}

template<bool THREADSAFE>
template<class T, typename A1>
inline T *ArenaBase<THREADSAFE>::NewObject(A1 arg1) {
  void *mem = AllocateBytes(sizeof(T));
  if (mem == NULL) throw std::bad_alloc();
  return new (mem) T(arg1);
}


template<bool THREADSAFE>
template<class T, typename A1, typename A2>
inline T *ArenaBase<THREADSAFE>::NewObject(A1 arg1, A2 arg2) {
  void *mem = AllocateBytes(sizeof(T));
  if (mem == NULL) throw std::bad_alloc();
  return new (mem) T(arg1, arg2);
}

template<bool THREADSAFE>
template<class T, typename A1, typename A2, typename A3>
inline T *ArenaBase<THREADSAFE>::NewObject(A1 arg1, A2 arg2, A3 arg3) {
  void *mem = AllocateBytes(sizeof(T));
  if (mem == NULL) throw std::bad_alloc();
  return new (mem) T(arg1, arg2, arg3);
}


}  // namespace supersonic

#endif  // SUPERSONIC_BASE_MEMORY_ARENA_H_
