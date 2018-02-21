#pragma once
#ifndef SIG_TREE_CODING_H
#define SIG_TREE_CODING_H

#include <cstdint>

namespace sgt {
    static_assert(sizeof(uint8_t) == sizeof(char));

    inline char Uint8ToChar(uint8_t i) {
        return *reinterpret_cast<char *>(&i);
    }

    inline uint8_t CharToUint8(char c) {
        return *reinterpret_cast<uint8_t *>(&c);
    }
}

#endif //SIG_TREE_CODING_H
