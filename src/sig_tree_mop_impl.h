#pragma once
#ifndef SIG_TREE_SIG_TREE_MOP_IMPL_H
#define SIG_TREE_SIG_TREE_MOP_IMPL_H

#ifndef SGT_NO_MM_PREFETCH
#include <xmmintrin.h>
#endif

#include "likely.h"
#include "sig_tree.h"

namespace sgt {
    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<size_t N, typename CALLBACK>
    auto SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    MultiGetWithCallback(const Slice * ks,
                         CALLBACK && callback) {
        std::array<KV_REP *, N> reps{};
        Node * root = OffsetToMemNode(kRootOffset);
        if (SGT_UNLIKELY(NodeSize(root) == 0)) {
            if constexpr (std::is_same<CALLBACK, std::false_type>::value) {
                return reps;
            } else {
                return callback(reps);
            }
        }

        std::array<Node *, N> cursors;
        cursors.fill(root);

        size_t remaining;
        do {
            remaining = N;

            for (size_t i = 0; i < N; ++i) {
                Node * cursor = cursors[i];
                if (cursor != nullptr) {
                    size_t idx;
                    bool direct;
                    std::tie(idx, direct, std::ignore) = FindBestMatchImpl(cursor, ks[i]);
#ifndef SGT_NO_MM_PREFETCH
                    _mm_prefetch(reps[i] = &cursor->reps_[idx + direct], _MM_HINT_T0);
#else
                    reps[i] = &cursor->reps_[idx + direct];
#endif
                }
            }

            for (size_t i = 0; i < N; ++i) {
                Node *& cursor = cursors[i];
                if (cursor != nullptr) {
                    const auto & rep = *reps[i];
                    if (IsPacked(rep)) {
                        cursor = OffsetToMemNode(Unpack(rep));
#ifndef SGT_NO_MM_PREFETCH
                        _mm_prefetch(&cursor->size_, _MM_HINT_T0);
                        auto p = reinterpret_cast<const char *>(&cursor->diffs_);
                        p -= reinterpret_cast<uintptr_t>(p) % 64;
                        _mm_prefetch(p + 64 * 0, _MM_HINT_T2);
                        _mm_prefetch(p + 64 * 1, _MM_HINT_T2);
                        _mm_prefetch(p + 64 * 2, _MM_HINT_T2);
                        _mm_prefetch(p + 64 * 3, _MM_HINT_T2);
                        _mm_prefetch(p + 64 * 4, _MM_HINT_T2);
#endif
                        continue;
                    }
                    cursor = nullptr;
                }
                --remaining;
            }
        } while (remaining != 0);

        if constexpr (std::is_same<CALLBACK, std::false_type>::value) {
            return reps;
        } else {
            return callback(reps);
        }
    }
}

#endif //SIG_TREE_SIG_TREE_MOP_IMPL_H
