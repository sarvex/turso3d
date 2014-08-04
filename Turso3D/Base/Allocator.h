// For conditions of distribution and use, see copyright notice in License.txt

#pragma once

#include "../Turso3DConfig.h"

#include <cstddef>
#include <new>

namespace Turso3D
{

struct AllocatorBlock;
struct AllocatorNode;

/// %Allocator memory block.
struct AllocatorBlock
{
    /// Size of a node.
    size_t nodeSize;
    /// Number of nodes in this block.
    size_t capacity;
    /// First free node.
    AllocatorNode* free;
    /// Next allocator block.
    AllocatorBlock* next;
    /// Nodes follow.
};

/// %Allocator node.
struct AllocatorNode
{
    /// Next free node.
    AllocatorNode* next;
    /// Data follows.
};

/// Initialize a fixed-size allocator with the node size and initial capacity.
TURSO3D_API AllocatorBlock* AllocatorInitialize(size_t nodeSize, size_t initialCapacity = 1);
/// Uninitialize a fixed-size allocator. Frees all blocks in the chain.
TURSO3D_API void AllocatorUninitialize(AllocatorBlock* allocator);
/// Allocate a node. Creates a new block if necessary.
TURSO3D_API void* AllocatorGet(AllocatorBlock* allocator);
/// Free a node. Does not free any blocks.
TURSO3D_API void AllocatorFree(AllocatorBlock* allocator, void* node);

/// %Allocator template class. Allocates objects of a specific class.
template <class T> class Allocator
{
public:
    /// Construct with optional initial capacity.
    Allocator(size_t capacity = 0) :
        allocator(0)
    {
        if (capacity)
            Reserve(capacity);
    }
    
    /// Destruct. All objects reserved from this allocator should be freed before this is called.
    ~Allocator()
    {
        AllocatorUninitialize(allocator);
    }
    
    /// Reserve initial capacity. Only possible before allocating the first object.
    void Reserve(size_t capacity)
    {
        if (allocator)
            allocator = AllocatorInitialize(sizeof(T), capacity);
    }

    /// Allocate and default-construct an object.
    T* Allocate()
    {
        if (!allocator)
            allocator = Allocator(sizeof(T));
        T* newObject = static_cast<T*>(AllocatorGet(allocator));
        new(newObject) T();
        
        return newObject;
    }
    
    /// Allocate and copy-construct an object.
    T* Allocate(const T& object)
    {
        if (!allocator)
            allocator = AllocatorInitialize(sizeof(T));
        T* newObject = static_cast<T*>(AllocatorGet(allocator));
        new(newObject) T(object);
        
        return newObject;
    }
    
    /// Destruct and free an object.
    void Free(T* object)
    {
        (object)->~T();
        AllocatorFree(allocator, object);
    }
    
private:
    /// Prevent copy construction.
    Allocator(const Allocator<T>& rhs);
    /// Prevent assignment.
    Allocator<T>& operator = (const Allocator<T>& rhs);
    
    /// Allocator block.
    AllocatorBlock* allocator;
};

}