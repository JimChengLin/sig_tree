#pragma once
#ifndef SIG_TREE_SLICE_H
#define SIG_TREE_SLICE_H

/*
 * 数据引用类
 */

#include <cassert>
#include <cstring>
#include <string>
#include <type_traits>

#include "coding.h"

namespace sgt {
    class Slice {
    private:
        const char * data_ = "";
        size_t size_ = 0;

    public:
        Slice() = default;

        Slice(const char * d, size_t n) : data_(d), size_(n) {}

        template<typename T>
        Slice(const T * s, size_t n)
                : Slice(reinterpret_cast<const char *>(s), n) {
            static_assert(sizeof(T) == sizeof(char));
        }

        template<typename T, typename = std::enable_if_t<std::is_class<T>::value>>
        Slice(const T & s) : Slice(s.data(), s.size()) {}

        template<typename T, typename = std::enable_if_t<std::is_convertible<T, const char *>::value>>
        Slice(T s) : data_(s), size_(strlen(s)) {}

        template<size_t L>
        Slice(const char (& s)[L]) : data_(s), size_(L - 1) {}

    public:
        // same as STL
        const char * data() const { return data_; }

        // same as STL
        size_t size() const { return size_; }

        const char & operator[](size_t n) const {
            assert(n < size_);
            return data_[n];
        }

        bool operator==(const Slice & another) const {
            return size_ == another.size_ && memcmp(data_, another.data_, size_) == 0;
        }

        bool operator!=(const Slice & another) const { return !operator==(another); }

        std::string_view ToStringView() const { return {data_, size_}; }

        std::string ToString() const { return {data_, size_}; }
    };

    inline int SliceCmp(const Slice & a, const Slice & b) {
        int r = memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
        if (r == 0) {
            r -= static_cast<int>(static_cast<ssize_t>(b.size()) - static_cast<ssize_t>(a.size()));
        }
        return r;
    }

    struct SliceComparator {
        using is_transparent = std::true_type;

        bool operator()(Slice a, Slice b) const {
            return SliceCmp(a, b) < 0;
        }
    };

    struct SliceHasher {
        std::size_t operator()(const Slice & s) const {
            return std::hash<std::string_view>()(s.ToStringView());
        }
    };

    template<typename O, typename S, typename = std::enable_if_t<std::is_same<S, Slice>::value>>
    inline O & operator<<(O & o, const S & s) {
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (isprint(c)) {
                o << c;
            } else {
                o << '['
                  << static_cast<unsigned int>(CharToUint8(c))
                  << ']';
            }
        }
        return o;
    }
}

#endif //SIG_TREE_SLICE_H