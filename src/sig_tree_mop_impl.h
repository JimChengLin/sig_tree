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
                    auto[idx, direct, _] = FindBestMatchImpl(cursor, ks[i]);
                    auto & rep = reps[i];
                    rep = &cursor->reps_[idx + direct];
#ifndef SGT_NO_MM_PREFETCH
                    _mm_prefetch(rep, _MM_HINT_T0);
#endif
                }
            }

            for (size_t i = 0; i < N; ++i) {
                Node *& cursor = cursors[i];
                if (cursor != nullptr) {
                    const auto & r = *reps[i];
                    if (IsPacked(r)) {
                        cursor = OffsetToMemNode(Unpack(r));
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
