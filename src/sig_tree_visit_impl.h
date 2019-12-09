#pragma once
#ifndef SIG_TREE_SIG_TREE_VISIT_IMPL_H
#define SIG_TREE_SIG_TREE_VISIT_IMPL_H

#include "autovector.h"
#include "likely.h"
#include "sig_tree.h"

namespace sgt {
    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<typename T, bool BACKWARD, typename VISITOR, typename E>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    VisitGenericImpl(T self, const Slice & target, VISITOR && visitor, E && expected) {
        Node * cursor = self->OffsetToMemNode(self->kRootOffset);
        if (SGT_UNLIKELY(NodeSize(cursor) == 0)) {
            return;
        }
        rocksdb::autovector<std::pair<Node *, size_t /* rep_idx */>, 16> que;

        [[maybe_unused]] auto leftmost = [self, &que](Node * cursor) {
            while (true) {
                que.emplace_back(cursor, 0);
                const auto & rep = cursor->reps_[0];
                if (self->IsPacked(rep)) {
                    cursor = self->OffsetToMemNode(self->Unpack(rep));
                } else {
                    break;
                }
            }
        };

        [[maybe_unused]] auto next = [self, &que, &leftmost]() {
            while (!que.empty()) {
                auto & p = que.back();
                if (++p.second < NodeSize(p.first)) {
                    const auto & rep = p.first->reps_[p.second];
                    if (self->IsPacked(rep)) {
                        leftmost(self->OffsetToMemNode(self->Unpack(rep)));
                    }
                    break;
                }
                que.pop_back();
            }
        };

        [[maybe_unused]] auto rightmost = [self, &que](Node * cursor) {
            while (true) {
                size_t rep_idx = NodeSize(cursor) - 1;
                que.emplace_back(cursor, rep_idx);
                const auto & rep = cursor->reps_[rep_idx];
                if (self->IsPacked(rep)) {
                    cursor = self->OffsetToMemNode(self->Unpack(rep));
                } else {
                    break;
                }
            }
        };

        [[maybe_unused]] auto prev = [self, &que, &rightmost]() {
            while (!que.empty()) {
                auto & p = que.back();
                if (p.second != 0) {
                    --p.second;
                    const auto & rep = p.first->reps_[p.second];
                    if (self->IsPacked(rep)) {
                        rightmost(self->OffsetToMemNode(self->Unpack(rep)));
                    }
                    break;
                }
                que.pop_back();
            }
        };

        if (target.size() == 0) {
            if constexpr (!BACKWARD) {
                leftmost(cursor);
            } else {
                rightmost(cursor);
            }
        } else { // Seek
            while (true) {
                auto[idx, direct, _] = FindBestMatch(cursor, target);
                size_t rep_idx = idx + direct;
                que.emplace_back(cursor, rep_idx);

                const auto & rep = cursor->reps_[rep_idx];
                if (self->IsPacked(rep)) {
                    cursor = self->OffsetToMemNode(self->Unpack(rep));
                } else {
                    if constexpr (!std::is_same<E, std::false_type>::value) {
                        if (expected == rep) {
                            break;
                        }
                    }

                    const auto & trans = self->helper_->Trans(rep);
                    if (trans == target) {
                    } else { // Reseek
                        que.pop_back();

                        [self, &que, &next](const Slice & opponent, const Slice & k,
                                            Node * hint, size_t hint_idx, bool hint_direct) {
                            K_DIFF diff_at = 0;
                            char a, b;
                            while ((a = opponent[diff_at]) == (b = k[diff_at])) {
                                ++diff_at;
                            }

                            // __builtin_clz: returns the number of leading 0-bits in x, starting at the
                            // most significant bit position if x is 0, the result is undefined
                            uint8_t shift = (__builtin_clz(CharToUint8(a ^ b)) ^ 31);  // bsr
                            auto direct = ((CharToUint8(b) >> shift) & 1);

                            K_DIFF packed_diff = PackDiffAtAndShift(diff_at, shift);
                            Node * cursor = hint;
                            restart:
                            while (true) {
                                size_t insert_idx;
                                bool insert_direct;

                                size_t cursor_size = NodeSize(cursor);
                                if (cursor_size == 1 || (hint != nullptr && packed_diff > hint->diffs_[hint_idx])) {
                                    insert_idx = hint_idx;
                                    insert_direct = hint_direct;
                                    assert(!self->IsPacked(cursor->reps_[insert_idx + insert_direct]));
                                } else {
                                    const K_DIFF * cbegin = cursor->diffs_.cbegin();
                                    const K_DIFF * cend = &cursor->diffs_[cursor_size - 1];

                                    K_DIFF exist_diff;
                                    auto pyramid = cursor->pyramid_;
                                    const K_DIFF * min_it = cbegin + cursor->pyramid_.MinAt(cbegin, cend, &exist_diff);
                                    while (true) {
                                        assert(min_it == std::min_element(cbegin, cend) && *min_it == exist_diff);
                                        if (exist_diff > packed_diff) {
                                            if (hint != nullptr) {
                                                hint = nullptr;
                                                cursor = self->OffsetToMemNode(self->kRootOffset);
                                                que.clear();
                                                goto restart;
                                            }
                                            insert_idx = (!direct ? cbegin : (cend - 1)) - cursor->diffs_.cbegin();
                                            insert_direct = direct;
                                            break;
                                        }
                                        hint = nullptr;

                                        auto[crit_diff_at, crit_shift] = UnpackDiffAtAndShift(exist_diff);
                                        uint8_t crit_byte = k.size() > crit_diff_at
                                                            ? CharToUint8(k[crit_diff_at])
                                                            : static_cast<uint8_t>(0);
                                        auto crit_direct = ((crit_byte >> crit_shift) & 1);
                                        if (!crit_direct) {
                                            cend = min_it;
                                            if (cbegin == cend) {
                                                insert_idx = min_it - cursor->diffs_.cbegin();
                                                insert_direct = crit_direct;
                                                break;
                                            }
                                            min_it = cursor->diffs_.cbegin() +
                                                     pyramid.TrimRight(cursor->diffs_.cbegin(), cbegin, cend,
                                                                       &exist_diff);
                                        } else {
                                            cbegin = min_it + 1;
                                            if (cbegin == cend) {
                                                insert_idx = min_it - cursor->diffs_.cbegin();
                                                insert_direct = crit_direct;
                                                break;
                                            }
                                            min_it = cursor->diffs_.cbegin() +
                                                     pyramid.TrimLeft(cursor->diffs_.cbegin(), cbegin, cend,
                                                                      &exist_diff);
                                        }
                                    }
                                }

                                size_t rep_idx = insert_idx + insert_direct;
                                que.emplace_back(cursor, rep_idx);

                                const auto & rep = cursor->reps_[rep_idx];
                                if (cursor->diffs_[insert_idx] > packed_diff || !self->IsPacked(rep)) {
                                    if (direct) {
                                        next();
                                    }
                                    break;
                                }
                                cursor = self->OffsetToMemNode(self->Unpack(rep));
                            }
                        }(trans.Key(), target, cursor, idx, direct);

                    }
                    break;
                }
            }
        }

        if constexpr (std::is_same<T, const SignatureTreeTpl *>::value) {
            while (!que.empty()) {
                auto & p = que.back();
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
        } else { // del
            while (!que.empty()) {
                auto it = que.end();
                auto[node, rep_idx] = *(--it);
                auto[proceed, del] = visitor(node->reps_[rep_idx]);

                if (proceed) {
                    if (del) {
                        Node * parent = nullptr;
                        size_t parent_rep_idx{};
                        size_t parent_size{};
                        if (it != que.begin()) {
                            std::tie(parent, parent_rep_idx) = *(--it);
                            parent_size = NodeSize(parent);
                        }

                        size_t size = NodeSize(node);
                        bool direct = !(rep_idx == 0 || (rep_idx != size - 1
                                                         && node->diffs_[rep_idx - 1] < node->diffs_[rep_idx]));

                        NodeRemove(node, rep_idx - direct, direct, size--);
                        if (parent != nullptr && parent->reps_.size() - parent_size + 1 >= size) {
                            self->NodeMerge(parent, parent_rep_idx, false, parent_size,
                                            node, size);
                            it->second += rep_idx;
                            que.pop_back();
                        } else if (KV_REP r;
                                size == 1 && (r = node->reps_[0], self->IsPacked(r))) {
                            Node * child = self->OffsetToMemNode(self->Unpack(r));
                            size_t child_size = NodeSize(child);
                            self->NodeMerge(node, 0, false, 1,
                                            child, child_size);
                            if (rep_idx == 0) {
                                if constexpr (!BACKWARD) {
                                } else {
                                    que.pop_back();
                                }
                            } else {
                                assert(rep_idx == 1);
                                if constexpr (!BACKWARD) {
                                    que.pop_back();
                                } else {
                                    it->second = child_size - 1;
                                }
                            }
                            continue;
                        }

                        if constexpr (!BACKWARD) {
                            std::tie(node, rep_idx) = que.back();
                            if (rep_idx < NodeSize(node)) {
                                const auto & rep = node->reps_[rep_idx];
                                if (self->IsPacked(rep)) {
                                    leftmost(self->OffsetToMemNode(self->Unpack(rep)));
                                }
                                continue;
                            }
                        }
                    }

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
}

#endif //SIG_TREE_SIG_TREE_VISIT_IMPL_H
