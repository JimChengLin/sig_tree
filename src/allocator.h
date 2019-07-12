#pragma once
#ifndef SIG_TREE_ALLOCATOR_H
#define SIG_TREE_ALLOCATOR_H

#include <exception>

namespace sgt {
    class AllocatorFullException : public std::exception {
    public:
        const char * what() const noexcept override {
            return "no space for allocation";
        }
    };

    class Allocator {
    public:
        Allocator() = default;

        virtual ~Allocator() = default;

    public:
        virtual void * Base() = 0;

        // 如无法分配, 抛出 AllocatorFullException
        virtual size_t AllocatePage() = 0;

        virtual void FreePage(size_t offset) = 0;

        virtual void Grow() = 0;
    };
}

#endif //SIG_TREE_ALLOCATOR_H
