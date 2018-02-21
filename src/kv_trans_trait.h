#pragma once
#ifndef SIG_TREE_KV_TRANS_TRAIT_H
#define SIG_TREE_KV_TRANS_TRAIT_H

#include <type_traits>

#include "slice.h"

namespace sgt {
    template<typename KV_TRANS>
    struct is_kv_trans {
        enum {
            value =
            std::is_same<decltype(std::declval<const KV_TRANS>() == Slice()), bool>::value &&
            std::is_same<decltype(std::declval<const KV_TRANS>().Key()), Slice>::value &&
            std::is_same<decltype(std::declval<const KV_TRANS>().Get(Slice(), (std::string *) {})), bool>::value,
        };
    };
}

#endif //SIG_TREE_KV_TRANS_TRAIT_H
