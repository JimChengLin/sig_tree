#pragma once
#ifndef SIG_TREE_KV_TRANS_TRAIT_H
#define SIG_TREE_KV_TRANS_TRAIT_H

#include <type_traits>

#include "slice.h"

#define SGT_CHECK_EXPR(Name, Expr)                                         \
template<typename U>                                                       \
struct Name {                                                              \
private:                                                                   \
    template<typename>                                                     \
    inline static constexpr std::false_type Check(...);                    \
                                                                           \
    template<typename T>                                                   \
    inline static constexpr decltype((Expr), std::true_type()) Check(int); \
                                                                           \
public:                                                                    \
    enum {                                                                 \
        value = decltype(Name<U>::Check<U>(int()))::value                  \
    };                                                                     \
}

namespace sgt {
    template<typename KV_TRANS>
    struct is_kv_trans {
        enum {
            value =
            std::is_same<decltype(std::declval<const KV_TRANS>() == Slice()), bool>::value &&
            std::is_same<decltype(std::declval<const KV_TRANS>().Key()), Slice>::value &&
            std::is_same<decltype(std::declval<const KV_TRANS>().Get(Slice(), (std::string *) {})), bool>::value
        };
    };

    SGT_CHECK_EXPR(has_pack, T::Pack);

    SGT_CHECK_EXPR(has_unpack, T::Unpack);

    SGT_CHECK_EXPR(has_is_packed, T::IsPacked);

    SGT_CHECK_EXPR(has_base, T::Base);
}

#endif //SIG_TREE_KV_TRANS_TRAIT_H
