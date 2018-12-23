#pragma once
#ifndef SIG_TREE_CODING_H
#define SIG_TREE_CODING_H

#include <cstdint>
#include <cstring>

namespace sgt {
    static_assert(sizeof(uint8_t) == sizeof(char));

    inline char Uint8ToChar(uint8_t i) {
        char c;
        memcpy(&c, &i, sizeof(c));
        return c;
    }

    inline uint8_t CharToUint8(char c) {
        uint8_t i;
        memcpy(&i, &c, sizeof(i));
        return i;
    }
}

#endif //SIG_TREE_CODING_H
