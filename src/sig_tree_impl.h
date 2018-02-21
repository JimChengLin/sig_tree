#pragma once
#ifndef SIG_TREE_SIG_TREE_IMPL_H
#define SIG_TREE_SIG_TREE_IMPL_H

#include "coding.h"
#include "likely.h"
#include "sig_tree.h"

namespace sgt {
    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    SignatureTreeTpl(Helper * helper, Allocator * allocator)
            : SignatureTreeTpl(helper, allocator, allocator->AllocatePage()) {
        auto * root = new(OffsetToMemNode(kRootOffset)) Node();
        std::fill(root->reps_.begin(), root->reps_.end(), kNullRep);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Get(const Slice & k, std::string * v) const {
        const Node * cursor = OffsetToMemNode(kRootOffset);

        while (true) {
            size_t idx;
            bool direct;
            std::tie(idx, direct, std::ignore) = FindBestMatch(cursor, k);

            const auto & rep = cursor->reps_[idx + direct];
            if (SGT_UNLIKELY(rep == kNullRep)) {
                return false;
            }
            if (helper_->IsPacked(rep)) {
                cursor = OffsetToMemNode(helper_->Unpack(rep));
            } else {
                const auto & trans = helper_->Trans(rep);
                return trans.Get(k, v);
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    size_t SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Size() const {
        auto SizeSub = [this](size_t offset, auto && SizeSub) -> size_t {
            size_t cnt = 0;
            const Node * cursor = OffsetToMemNode(offset);
            for (const auto & rep:cursor->reps_) {
                if (rep == kNullRep) {
                    break;
                }
                if (helper_->IsPacked(rep)) {
                    cnt += SizeSub(helper_->Unpack(rep), SizeSub);
                } else {
                    ++cnt;
                }
            }
            return cnt;
        };
        return SizeSub(kRootOffset, SizeSub);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Add(const Slice & k, const Slice & v,
        const std::function<bool(KV_TRANS &, KV_REP &)> & if_dup_callback) {
        assert(k.size() < GetMaxKeyLength());
        Node * cursor = OffsetToMemNode(kRootOffset);

        while (true) {
            size_t idx;
            bool direct;
            std::tie(idx, direct, std::ignore) = FindBestMatch(cursor, k);

            auto & rep = cursor->reps_[idx + direct];
            if (SGT_UNLIKELY(rep == kNullRep)) {
                *(&rep) = helper_->Add(k, v);
                return true;
            }
            if (helper_->IsPacked(rep)) {
                cursor = OffsetToMemNode(helper_->Unpack(rep));
            } else {
                auto && trans = helper_->Trans(rep);
                if (trans == k) {
                    if (if_dup_callback != nullptr) {
                        return if_dup_callback(trans, rep);
                    } else { // cannot overwrite by default
                        return false;
                    }
                } else { // insert
                    return CombatInsert(trans.Key(), k, v);
                }
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Del(const Slice & k) {
        Node * parent = nullptr;
        Node * cursor = OffsetToMemNode(kRootOffset);
        size_t parent_idx;
        bool parent_direct;
        size_t parent_size;

        while (true) {
            size_t idx;
            bool direct;
            size_t size;
            std::tie(idx, direct, size) = FindBestMatch(cursor, k);

            const auto & rep = cursor->reps_[idx + direct];
            if (SGT_UNLIKELY(rep == kNullRep)) {
                return false;
            }
            if (helper_->IsPacked(rep)) {
                parent = cursor;
                parent_idx = idx;
                parent_direct = direct;
                parent_size = size;
                cursor = OffsetToMemNode(helper_->Unpack(rep));
            } else {
                auto && trans = helper_->Trans(rep);
                if (trans == k) {
                    helper_->Del(trans);
                    NodeRemove(cursor, idx, direct, size);
                    if (parent != nullptr && parent->reps_.size() - parent_size + 1 >= --size) {
                        NodeMerge(parent, parent_idx, parent_direct, parent_size,
                                  cursor, size);
                    }
                    return true;
                } else {
                    return false;
                }
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    std::tuple<size_t, bool, size_t>
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    FindBestMatch(const Node * node, const Slice & k) const {
        const K_DIFF * cbegin = node->diffs_.cbegin();
        const K_DIFF * cend;

        size_t size = NodeSize(node);
        if (SGT_UNLIKELY(size <= 1)) {
            return {0, false, size};
        }
        size_t diffs_size = size - 1;
        cend = &node->diffs_[diffs_size];

        if (node->dirty_) {
            const_cast<Node *>(node)->pyramid_.Build(cbegin, cend);
            const_cast<Node *>(node)->dirty_ = false;
        }

        auto pyramid = node->pyramid_;
        const K_DIFF * min_it = cbegin + pyramid.MinAt(cbegin, cend);
        while (true) {
            assert(min_it == std::min_element(cbegin, cend));
            K_DIFF diff_at;
            uint8_t shift;
            UnpackDiffAtAndShift(*min_it, &diff_at, &shift);
            uint8_t mask = ~(static_cast<uint8_t>(1) << shift);

            // left or right?
            uint8_t crit_byte = k.size() > diff_at
                                ? CharToUint8(k[diff_at])
                                : static_cast<uint8_t>(0);
            auto direct = static_cast<bool>((1 + (crit_byte | mask)) >> 8);
            if (!direct) { // go left
                cend = min_it;
                if (cbegin == cend) {
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimRight(node->diffs_.cbegin(), cbegin, cend);
            } else { // go right
                cbegin = min_it + 1;
                if (cbegin == cend) {
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimLeft(node->diffs_.cbegin(), cbegin, cend);
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    CombatInsert(const Slice & opponent, const Slice & k, const Slice & v) {
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
        Node * cursor = OffsetToMemNode(kRootOffset);
        while (true) {
            size_t insert_idx;
            bool insert_direct;

            size_t cursor_size = NodeSize(cursor);
            if (SGT_UNLIKELY(cursor_size == 1)) {
                insert_idx = 0;
                insert_direct = false;
            } else {
                size_t cursor_diffs_size = cursor_size - 1;
                const K_DIFF * cbegin = cursor->diffs_.cbegin();
                const K_DIFF * cend = &cursor->diffs_[cursor_diffs_size];

                if (cursor->dirty_) {
                    cursor->pyramid_.Build(cbegin, cend);
                    cursor->dirty_ = false;
                }

                auto pyramid = cursor->pyramid_;
                const K_DIFF * min_it = cbegin + pyramid.MinAt(cbegin, cend);
                while (true) {
                    assert(min_it == std::min_element(cbegin, cend));
                    if (*min_it > packed_diff) {
                        if (!direct) {
                            insert_idx = cbegin - cursor->diffs_.cbegin();
                        } else {
                            insert_idx = cend - cursor->diffs_.cbegin() - 1;
                        }
                        insert_direct = direct;
                        break;
                    }

                    K_DIFF crit_diff_at;
                    uint8_t crit_shift;
                    UnpackDiffAtAndShift(*min_it, &crit_diff_at, &crit_shift);
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

            const auto & rep = cursor->reps_[insert_idx + insert_direct];
            if (cursor->diffs_[insert_idx] > packed_diff || !helper_->IsPacked(rep)) {
                if (IsNodeFull(cursor)) {
                    try {
                        NodeSplit(cursor);
                    } catch (const AllocatorFullException &) {
                        size_t offset = reinterpret_cast<uintptr_t>(cursor) -
                                        reinterpret_cast<uintptr_t>(allocator_->Base());
                        allocator_->Grow();
                        cursor = OffsetToMemNode(offset);
                        NodeSplit(cursor);
                    }
                    continue;
                }
                NodeInsert(cursor, insert_idx, insert_direct, direct, packed_diff, helper_->Add(k, v), cursor_size);
                break;
            }
            cursor = OffsetToMemNode(helper_->Unpack(rep));
        }
        return true;
    }

#define add_gap(arr, idx, size) memmove(&(arr)[(idx) + 1], &(arr)[(idx)], sizeof((arr)[0]) * ((size) - (idx)))
#define del_gap(arr, idx, size) memmove(&(arr)[(idx)], &(arr)[(idx) + 1], sizeof((arr)[0]) * ((size) - ((idx) + 1)))
#define add_gaps(arr, idx, size, n) memmove(&(arr)[(idx) + (n)], &(arr)[(idx)], sizeof((arr)[0]) * ((size) - (idx)))
#define del_gaps(arr, idx, size, n) memmove(&(arr)[(idx)], &(arr)[(idx) + (n)], sizeof((arr)[0]) * ((size) - ((idx) + (n))))
#define cpy_part(dst, dst_idx, src, src_idx, n) memcpy(&(dst)[(dst_idx)], &(src)[(src_idx)], sizeof((src)[0]) * (n))

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeSplit(Node * parent) {
        size_t offset = allocator_->AllocatePage(); // may throw AllocatorFullException
        Node * child = new(OffsetToMemNode(offset)) Node();

        // find nearly half
        const K_DIFF * cbegin = parent->diffs_.cbegin();
        const K_DIFF * cend = parent->diffs_.cend();

        auto pyramid = parent->pyramid_;
        const K_DIFF * min_it = cbegin + pyramid.MinAt(cbegin, cend);
        while (true) {
            assert(min_it == std::min_element(cbegin, cend));
            if (min_it - cbegin <= cend - min_it) { // go right
                cbegin = min_it + 1;
                if (cend - cbegin <= parent->diffs_.size() / 2) {
                    break;
                }
                min_it = parent->diffs_.cbegin() +
                         pyramid.TrimLeft(parent->diffs_.cbegin(), cbegin, cend);
            } else { // go left
                cend = min_it;
                if (cend - cbegin <= parent->diffs_.size() / 2) {
                    break;
                }
                min_it = parent->diffs_.cbegin() +
                         pyramid.TrimRight(parent->diffs_.cbegin(), cbegin, cend);
            }
        }

        size_t item_num = cend - cbegin;
        size_t nth = cbegin - parent->diffs_.cbegin();

        cpy_part(child->diffs_, 0, parent->diffs_, nth, item_num);
        cpy_part(child->reps_, 0, parent->reps_, nth, item_num + 1);
        std::fill(child->reps_.begin() + (item_num + 1), child->reps_.end(), kNullRep);

        del_gaps(parent->diffs_, nth, parent->diffs_.size(), item_num);
        del_gaps(parent->reps_, nth + 1, parent->reps_.size(), item_num);
        parent->reps_[nth] = helper_->Pack(offset);
        std::fill(parent->reps_.end() - item_num, parent->reps_.end(), kNullRep);

        assert(NodeSize(parent) == parent->reps_.size() - item_num);
        parent->dirty_ = true;
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeMerge(Node * parent, size_t idx, bool direct, size_t parent_size,
              Node * child, size_t child_size) {
        idx += static_cast<size_t>(direct);
        size_t offset = helper_->Unpack(parent->reps_[idx]);

        add_gaps(parent->diffs_, idx, parent_size - 1, child_size - 1);
        add_gaps(parent->reps_, idx + 1, parent_size, child_size - 1);

        cpy_part(parent->diffs_, idx, child->diffs_, 0, child_size - 1);
        cpy_part(parent->reps_, idx, child->reps_, 0, child_size);

        allocator_->FreePage(offset);
        parent->dirty_ = true;
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeInsert(Node * node, size_t insert_idx, bool insert_direct,
               bool direct, K_DIFF diff, const KV_REP & rep, size_t size) {
        assert(!IsNodeFull(node));
        insert_idx += insert_direct;
        size_t rep_idx = insert_idx + direct;

        add_gap(node->diffs_, insert_idx, size - 1);
        add_gap(node->reps_, rep_idx, size);

        node->diffs_[insert_idx] = diff;
        node->reps_[rep_idx] = rep;
        node->dirty_ = true;
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeRemove(Node * node, size_t idx, bool direct, size_t size) {
        assert(size >= 1);
        if (SGT_LIKELY(size > 1)) {
            del_gap(node->diffs_, idx, size - 1);
        }
        del_gap(node->reps_, idx + direct, size);

        node->reps_[size - 1] = kNullRep;
        node->dirty_ = true;
    }

#undef add_gap
#undef del_gap
#undef add_gaps
#undef del_gaps
#undef cpy_part
}

#endif //SIG_TREE_SIG_TREE_IMPL_H
