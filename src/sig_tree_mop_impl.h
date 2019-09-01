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
        struct GetContext {
            enum State {
                kFindBestMatchImpl = 0,
                kIsPacked,
                kEnd
            };

            int state;
            Node * cursor;
            KV_REP * rep;
            Slice k;

            inline static void Init(GetContext * ctx, SignatureTreeTpl * tree, const Slice & k) {
                ctx->state = kFindBestMatchImpl;
                ctx->cursor = tree->OffsetToMemNode(tree->kRootOffset);
                ctx->rep = nullptr;
                ctx->k = k;

                if (SGT_UNLIKELY(NodeSize(ctx->cursor) == 0)) {
                    ctx->state = kEnd;
                }
            }

            inline static void Next(GetContext * ctx, SignatureTreeTpl * tree) {
                switch (ctx->state) {
                    case kFindBestMatchImpl: {
                        size_t idx;
                        bool direct;
                        std::tie(idx, direct, std::ignore) = FindBestMatchImpl(ctx->cursor, ctx->k);
                        ctx->rep = &ctx->cursor->reps_[idx + direct];
#ifndef SGT_NO_MM_PREFETCH
                        _mm_prefetch(ctx->rep, _MM_HINT_T0);
#endif
                        break;
                    }

                    case kIsPacked: {
                        auto & r = *ctx->rep;
                        if (tree->IsPacked(r)) {
                            ctx->cursor = tree->OffsetToMemNode(tree->Unpack(r));
#ifndef SGT_NO_MM_PREFETCH
                            _mm_prefetch(&ctx->cursor->size_, _MM_HINT_T0);
                            auto p = reinterpret_cast<const char *>(&ctx->cursor->diffs_);
                            p -= reinterpret_cast<uintptr_t>(p) % 64;
                            _mm_prefetch(p + 64 * 0, _MM_HINT_T2);
                            _mm_prefetch(p + 64 * 1, _MM_HINT_T2);
                            _mm_prefetch(p + 64 * 2, _MM_HINT_T2);
                            _mm_prefetch(p + 64 * 3, _MM_HINT_T2);
                            _mm_prefetch(p + 64 * 4, _MM_HINT_T2);
#endif
                            ctx->state = kFindBestMatchImpl;
                            return;
                        }
                        break;
                    }

                    default:
                        assert(ctx->state == kEnd);
                        return;
                }
                ++ctx->state;
            }
        };

        std::array<GetContext, N> ctxs;
        for (size_t i = 0; i < N; ++i) {
            GetContext::Init(&ctxs[i], this, ks[i]);
        }

        size_t remaining;
        do {
            remaining = N;
            for (auto & ctx :ctxs) {
                GetContext::Next(&ctx, this);
                remaining -= (ctx.state == GetContext::kEnd);
            }
        } while (remaining != 0);

        std::array<KV_REP *, N> reps;
        for (size_t i = 0; i < N; ++i) { reps[i] = ctxs[i].rep; }
        if constexpr (std::is_same<CALLBACK, std::false_type>::value) {
            return reps;
        } else {
            return callback(reps);
        }
    }
}

#endif //SIG_TREE_SIG_TREE_MOP_IMPL_H
