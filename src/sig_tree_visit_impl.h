#pragma once
#ifndef SIG_TREE_SIG_TREE_VISIT_IMPL_H
#define SIG_TREE_SIG_TREE_VISIT_IMPL_H

#include "autovector.h"
#include "likely.h"
#include "sig_tree.h"

namespace sgt {
    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<bool BACKWARD, typename VISITOR>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Visit(const Slice & target, VISITOR && visitor) const {
        const Node * cursor = OffsetToMemNode(kRootOffset);
        if (SGT_UNLIKELY(NodeSize(cursor) == 0)) {
            return;
        }
        rocksdb::autovector<std::pair<const Node *, size_t /* rep_idx */>, 16> que;

        auto leftmost = [this, &que](const Node * cursor) {
            while (true) {
                que.emplace_back(cursor, 0);
                const auto & rep = cursor->reps_[0];
                if (helper_->IsPacked(rep)) {
                    cursor = OffsetToMemNode(helper_->Unpack(rep));
                } else {
                    break;
                }
            }
        };

        auto next = [this, &que, &leftmost]() {
            do {
                auto & p = que.back();
                if (++p.second != NodeSize(p.first)) {
                    const auto & rep = p.first->reps_[p.second];
                    if (helper_->IsPacked(rep)) {
                        leftmost(OffsetToMemNode(helper_->Unpack(rep)));
                    }
                    break;
                }
                que.pop_back();
            } while (!que.empty());
        };

        auto rightmost = [this, &que](const Node * cursor) {
            while (true) {
                size_t rep_idx = NodeSize(cursor) - 1;
                que.emplace_back(cursor, rep_idx);
                const auto & rep = cursor->reps_[rep_idx];
                if (helper_->IsPacked(rep)) {
                    cursor = OffsetToMemNode(helper_->Unpack(rep));
                } else {
                    break;
                }
            }
        };

        auto prev = [this, &que, &rightmost]() {
            do {
                auto & p = que.back();
                if (p.second != 0) {
                    --p.second;
                    const auto & rep = p.first->reps_[p.second];
                    if (helper_->IsPacked(rep)) {
                        rightmost(OffsetToMemNode(helper_->Unpack(rep)));
                    }
                    break;
                }
                que.pop_back();
            } while (!que.empty());
        };

        if (target.size() == 0) {
            if constexpr (!BACKWARD) {
                leftmost(cursor);
            } else {
                rightmost(cursor);
            }
        } else { // Seek
            while (true) {
                size_t idx;
                bool direct;
                std::tie(idx, direct, std::ignore) = FindBestMatch(cursor, target);

                size_t rep_idx = idx + direct;
                que.emplace_back(cursor, rep_idx);

                const auto & rep = cursor->reps_[rep_idx];
                if (helper_->IsPacked(rep)) {
                    cursor = OffsetToMemNode(helper_->Unpack(rep));
                } else {
                    const auto & trans = helper_->Trans(rep);
                    if (trans == target) {
                    } else { // Reseek
                        que.pop_back();

                        [this, &que, &next, &prev](const Slice & opponent, const Slice & k,
                                                   const Node * hint, size_t hint_idx, bool hint_direct) {
                            K_DIFF diff_at = 0;
                            while (opponent[diff_at] == k[diff_at]) {
                                ++diff_at;
                            }

                            // __builtin_clz: returns the number of leading 0-bits in x, starting at the most significant bit position
                            // if x is 0, the result is undefined
                            uint8_t shift = CHAR_BIT * sizeof(unsigned int)
                                            - __builtin_clz(CharToUint8(opponent[diff_at] ^ k[diff_at])) - 1;
                            uint8_t mask = ~(static_cast<uint8_t>(1) << shift);
                            auto direct = static_cast<bool>((1 + (CharToUint8(k[diff_at]) | mask)) >> 8);

                            K_DIFF packed_diff = PackDiffAtAndShift(diff_at, shift);
                            const Node * cursor = hint;
                            restart:
                            while (true) {
                                size_t insert_idx;
                                bool insert_direct;

                                size_t cursor_size = NodeSize(cursor);
                                if (SGT_UNLIKELY(cursor_size == 1)) {
                                    insert_idx = 0;
                                    insert_direct = false;
                                } else if (hint != nullptr && packed_diff > hint->diffs_[hint_idx]) {
                                    insert_idx = hint_idx;
                                    insert_direct = hint_direct;
                                    hint = nullptr;
                                } else {
                                    const K_DIFF * cbegin = cursor->diffs_.cbegin();
                                    const K_DIFF * cend = &cursor->diffs_[cursor_size - 1];

                                    auto pyramid = cursor->pyramid_;
                                    const K_DIFF * min_it = cbegin + pyramid.MinAt(cbegin, cend);
                                    while (true) {
                                        assert(min_it == std::min_element(cbegin, cend));
                                        K_DIFF exist_diff = *min_it;
                                        if (exist_diff > packed_diff) {
                                            if (hint != nullptr) {
                                                hint = nullptr;
                                                cursor = OffsetToMemNode(kRootOffset);
                                                que.clear();
                                                goto restart;
                                            }
                                            if (!direct) {
                                                insert_idx = cbegin - cursor->diffs_.cbegin();
                                            } else {
                                                insert_idx = cend - cursor->diffs_.cbegin() - 1;
                                            }
                                            insert_direct = direct;
                                            break;
                                        }
                                        hint = nullptr;

                                        K_DIFF crit_diff_at;
                                        uint8_t crit_shift;
                                        std::tie(crit_diff_at, crit_shift) = UnpackDiffAtAndShift(exist_diff);
                                        uint8_t crit_mask = ~(static_cast<uint8_t>(1) << crit_shift);

                                        uint8_t crit_byte = k.size() > crit_diff_at
                                                            ? CharToUint8(k[crit_diff_at])
                                                            : static_cast<uint8_t>(0);
                                        auto crit_direct = static_cast<bool>((1 + (crit_byte | crit_mask)) >> 8);
                                        if (!crit_direct) {
                                            cend = min_it;
                                            if (cbegin == cend) {
                                                insert_idx = min_it - cursor->diffs_.cbegin();
                                                insert_direct = crit_direct;
                                                break;
                                            }
                                            min_it = cursor->diffs_.cbegin() +
                                                     pyramid.TrimRight(cursor->diffs_.cbegin(), cbegin, cend);
                                        } else {
                                            cbegin = min_it + 1;
                                            if (cbegin == cend) {
                                                insert_idx = min_it - cursor->diffs_.cbegin();
                                                insert_direct = crit_direct;
                                                break;
                                            }
                                            min_it = cursor->diffs_.cbegin() +
                                                     pyramid.TrimLeft(cursor->diffs_.cbegin(), cbegin, cend);
                                        }
                                    }
                                }

                                size_t rep_idx = insert_idx + insert_direct;
                                que.emplace_back(cursor, rep_idx);

                                const auto & rep = cursor->reps_[rep_idx];
                                if (cursor->diffs_[insert_idx] > packed_diff || !helper_->IsPacked(rep)) {
                                    if (direct) {
                                        next();
                                    }
                                    break;
                                }
                                cursor = OffsetToMemNode(helper_->Unpack(rep));
                            }
                        }(trans.Key(), target, cursor, idx, direct);

                    }
                    break;
                }
            }
        }

        while (!que.empty()) {
            const auto & p = que.back();
            if (visitor(p.first->reps_[p.second])) {
                if constexpr (!BACKWARD) {
                    next();
                } else {
                    prev();
                }
            } else {
                break;
            }
        }
    }
}

#endif //SIG_TREE_SIG_TREE_VISIT_IMPL_H