#pragma once
#ifndef SIG_TREE_SIG_TREE_IMPL_H
#define SIG_TREE_SIG_TREE_IMPL_H

#ifndef SGT_NO_MM_PREFETCH
#include <xmmintrin.h>
#endif

#include <algorithm>

#include "coding.h"
#include "likely.h"
#include "sig_tree.h"

namespace sgt {
    template<typename T>
    inline const T * SmartMinElem8(const T * from, const T * to, T * min_val);

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    SignatureTreeTpl(Helper * helper, Allocator * allocator)
            : SignatureTreeTpl(helper, allocator, allocator->AllocatePage()) {
        new(OffsetToMemNode(kRootOffset)) Node();
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Get(const Slice & k, std::string * v) const {
        const Node * cursor = OffsetToMemNode(kRootOffset);
        if (SGT_UNLIKELY(NodeSize(cursor) == 0)) {
            return false;
        }

        while (true) {
            auto[idx, direct, _] = FindBestMatch(cursor, k);
            const auto & rep = cursor->reps_[idx + direct];
            if (IsPacked(rep)) {
                cursor = OffsetToMemNode(Unpack(rep));
            } else {
                const auto & trans = helper_->Trans(rep);
                return trans.Get(k, v);
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<typename CALLBACK>
    auto SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    GetWithCallback(const Slice & k,
                    CALLBACK && callback) {
        Node * cursor = OffsetToMemNode(kRootOffset);
        if (SGT_UNLIKELY(NodeSize(cursor) == 0)) {
            if constexpr (std::is_same<CALLBACK, std::false_type>::value) {
                return static_cast<KV_REP *>(nullptr);
            } else {
                return callback(static_cast<KV_REP *>(nullptr));
            }
        }

        while (true) {
            auto[idx, direct, _] = FindBestMatch(cursor, k);
            auto & r = cursor->reps_[idx + direct];
            if (IsPacked(r)) {
                cursor = OffsetToMemNode(Unpack(r));
            } else {
                if constexpr (std::is_same<CALLBACK, std::false_type>::value) {
                    return &r;
                } else {
                    return callback(&r);
                }
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    size_t SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Size() const {
        auto SizeSub = [this](size_t offset, auto && SizeSub) -> size_t {
            size_t cnt = 0;
            const Node * cursor = OffsetToMemNode(offset);
            for (size_t i = 0; i < NodeSize(cursor); ++i) {
                const auto & rep = cursor->reps_[i];
                if (IsPacked(rep)) {
                    cnt += SizeSub(Unpack(rep), SizeSub);
                } else {
                    ++cnt;
                }
            }
            return cnt;
        };
        return SizeSub(kRootOffset, SizeSub);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<typename V, typename IF_DUP_CALLBACK>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Add(const Slice & k, V && v,
        IF_DUP_CALLBACK && if_dup_callback) {
        assert(k.size() < kMaxKeyLength);
        Node * cursor = OffsetToMemNode(kRootOffset);
        if (SGT_UNLIKELY(NodeSize(cursor) == 0)) {
            if constexpr (std::is_convertible<V, KV_REP>::value) {
                cursor->reps_[0] = v;
            } else {
                cursor->reps_[0] = helper_->Add(k, std::forward<V>(v));
            }
            cursor->size_ = 1;
            return true;
        }

        while (true) {
            auto[idx, direct, _] = FindBestMatch(cursor, k);
            auto & rep = cursor->reps_[idx + direct];
            if (IsPacked(rep)) {
                cursor = OffsetToMemNode(Unpack(rep));
            } else {
                auto && trans = helper_->Trans(rep);
                if (trans == k) {
                    if constexpr (!std::is_same<IF_DUP_CALLBACK, std::false_type>::value) {
                        return if_dup_callback(trans, rep);
                    } else { // cannot overwrite by default
                        return false;
                    }
                } else { // insert
                    if constexpr (std::is_convertible<V, KV_REP>::value) {
                        return CombatInsert(trans.Key(), k, v,
                                            cursor, idx, direct);
                    } else {
                        return CombatInsert(trans.Key(), k, helper_->Add(k, std::forward<V>(v)),
                                            cursor, idx, direct);
                    }
                }
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Del(const Slice & k) {
        Node * cursor = OffsetToMemNode(kRootOffset);
        if (SGT_UNLIKELY(NodeSize(cursor) == 0)) {
            return false;
        }

        Node * parent = nullptr;
        size_t parent_idx{};
        bool parent_direct{};
        size_t parent_size{};

        while (true) {
            auto[idx, direct, size] = FindBestMatch(cursor, k);
            const auto & rep = cursor->reps_[idx + direct];
            if (IsPacked(rep)) {
                parent = cursor;
                parent_idx = idx;
                parent_direct = direct;
                parent_size = size;
                cursor = OffsetToMemNode(Unpack(rep));
            } else {
                auto && trans = helper_->Trans(rep);
                if (trans == k) {
                    helper_->Del(trans);
                    NodeRemove(cursor, idx, direct, size--);
                    if (parent != nullptr && parent->reps_.size() - parent_size + 1 >= size) {
                        NodeMerge(parent, parent_idx, parent_direct, parent_size,
                                  cursor, size);
                    } else if (KV_REP r;
                            size == 1 && (r = cursor->reps_[0], IsPacked(r))) {
                        assert(parent == nullptr);
                        Node * child = OffsetToMemNode(Unpack(r));
                        NodeMerge(cursor, 0, false, 1,
                                  child, NodeSize(child));
                    }
                    return true;
                } else {
                    return false;
                }
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Compact() {
        NodeCompact(OffsetToMemNode(kRootOffset));
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    std::tuple<size_t, bool, size_t>
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    FindBestMatch(const Node * node, const Slice & k) {
#ifndef SGT_NO_MM_PREFETCH
        _mm_prefetch(&node->size_, _MM_HINT_T0);
        auto p = reinterpret_cast<const char *>(&node->diffs_);
        p -= reinterpret_cast<uintptr_t>(p) % 64;
        _mm_prefetch(p + 64 * 0, _MM_HINT_T2);
        _mm_prefetch(p + 64 * 1, _MM_HINT_T2);
        _mm_prefetch(p + 64 * 2, _MM_HINT_T2);
        _mm_prefetch(p + 64 * 3, _MM_HINT_T2);
        _mm_prefetch(p + 64 * 4, _MM_HINT_T2);
#endif
        return FindBestMatchImpl(node, k);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    std::tuple<size_t, bool, size_t>
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    FindBestMatchImpl(const Node * node, const Slice & k) {
        size_t size = NodeSize(node);
        if (SGT_UNLIKELY(size <= 1)) {
            return {0, false, size};
        }

#ifndef SGT_NO_DENSE_INPUT_CACHE
        uint8_t diff_a;
        uint8_t diff_b;
        unsigned int diff_m;
        unsigned int diff_n;
        typename Node::Pyramid pyramid;
        const K_DIFF * base;
        K_DIFF base_val;

        const K_DIFF * cbegin = node->diffs_.cbegin();
        const K_DIFF * cend = &node->diffs_[size - 1];

        K_DIFF min_val;
        const K_DIFF * min_it = cbegin + node->pyramid_.MinAt(cbegin, cend, &min_val);
        auto[diff_at, shift] = UnpackDiffAtAndShift(min_val);

        uint8_t crit_byte = k.size() > diff_at
                            ? CharToUint8(k[diff_at])
                            : static_cast<uint8_t>(0);
        unsigned int pos = (crit_byte & ((1 << (shift + 1)) - 1));
        if (shift < 3) {
            ++diff_at;
            uint8_t remaining = 3 - shift;
            pos <<= remaining;
            pos |= ((k.size() > diff_at
                     ? CharToUint8(k[diff_at])
                     : static_cast<uint8_t>(0)) >> (8 - remaining));
        } else if (shift > 3) {
            pos >>= (shift - 3);
        }

        auto direct = (pos >> 3);
        auto & entry = const_cast<typename Node::Cache &>(node->cache_)[pos];
#define entry_as_ar entry.as_uint8_array
#define entry_as_ui entry.as_uint16

        if (entry_as_ui > 1) {
            const K_DIFF * cb;
            const K_DIFF * ce;
            if (!direct) { // left
                ce = min_it - entry_as_ar[0];
                cb = ce - entry_as_ar[1];
            } else { // right
                cb = min_it + entry_as_ar[0];
                ce = cb + entry_as_ar[1];
            }

            if (entry_as_ar[1] == 9) {
                const auto it = &cb[8];
                const auto val = cb[8];
                min_it = SmartMinElem8(cb, it, &min_val);
                if (min_val > val) {
                    min_val = val;
                    min_it = it;
                }
            } else if (entry_as_ar[1] <= 8) {
                min_it = SmartMinElem8(cb, ce, &min_val);
            } else {
                pyramid = node->pyramid_;
                if (cb != cbegin) {
                    min_it = node->diffs_.cbegin() + pyramid.TrimLeft(node->diffs_.cbegin(), cb, ce, &min_val);
                }
                if (ce != cend) {
                    min_it = node->diffs_.cbegin() + pyramid.TrimRight(node->diffs_.cbegin(), cb, ce, &min_val);
                }
            }

            cbegin = cb;
            cend = ce;
        } else if (auto it = min_it + direct;
                it == cbegin || // !direct && min_it - direct == cbegin
                it == cend) {   //  direct && min_it + direct == cend
            return {min_it - node->diffs_.cbegin(), direct, size};
        } else {
            pyramid = node->pyramid_;

            if (entry_as_ui != 0) {
                goto search_skip;
            }
            diff_a = 1;
            diff_b = 0;
            base = min_it;
            base_val = min_val;
            if (!direct) {
                goto build_cache_left;
            } else {
                goto build_cache_right;
            }
        }

        while (true) {
            search:
            assert(min_it == std::min_element(cbegin, cend) && *min_it == min_val);
            std::tie(diff_at, shift) = UnpackDiffAtAndShift(min_val);

            // left or right?
            crit_byte = k.size() > diff_at
                        ? CharToUint8(k[diff_at])
                        : static_cast<uint8_t>(0);
            direct = ((crit_byte >> shift) & 1);
            search_skip:
            if (!direct) { // go left
                cend = min_it;
                if (cbegin == cend) {
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimRight(node->diffs_.cbegin(), cbegin, cend, &min_val);
            } else { // go right
                cbegin = min_it + 1;
                if (cbegin == cend) {
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimLeft(node->diffs_.cbegin(), cbegin, cend, &min_val);
            }
        }

        while (true) {
            assert(min_it == std::min_element(cbegin, cend) && *min_it == min_val);
            std::tie(diff_at, shift) = UnpackDiffAtAndShift(min_val);

            // left or right?
            crit_byte = k.size() > diff_at
                        ? CharToUint8(k[diff_at])
                        : static_cast<uint8_t>(0);
            direct = ((crit_byte >> shift) & 1);
            if (!direct) { // go left
                build_cache_left:
                cend = min_it;
                if (cbegin == cend) {
                    if ((diff_m = static_cast<unsigned int>(base - (cend + 1) /* can be negative */)) <= UINT8_MAX) {
                        diff_a = diff_m;
                        diff_b = 1;
                    }
                    entry_as_ui = typename Node::CacheEntry{{diff_a, diff_b}}.as_uint16;
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimRight(node->diffs_.cbegin(), cbegin, cend, &min_val);
            } else { // go right
                cbegin = min_it + 1;
                if (cbegin == cend) {
                    if ((diff_m = static_cast<unsigned int>(base - cend)) <= UINT8_MAX) {
                        diff_a = diff_m;
                        diff_b = 1;
                    }
                    entry_as_ui = typename Node::CacheEntry{{diff_a, diff_b}}.as_uint16;
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimLeft(node->diffs_.cbegin(), cbegin, cend, &min_val);
            }

            if ((diff_m = static_cast<unsigned int>(base - cend)) <= UINT8_MAX &&
                (diff_n = cend - cbegin) <= UINT8_MAX) {
                diff_a = diff_m;
                diff_b = diff_n;
            }
            if (min_val - base_val >= 4) {
                entry_as_ui = typename Node::CacheEntry{{diff_a, diff_b}}.as_uint16;
                goto search;
            }
        }

        while (true) {
            assert(min_it == std::min_element(cbegin, cend) && *min_it == min_val);
            std::tie(diff_at, shift) = UnpackDiffAtAndShift(min_val);

            // left or right?
            crit_byte = k.size() > diff_at
                        ? CharToUint8(k[diff_at])
                        : static_cast<uint8_t>(0);
            direct = ((crit_byte >> shift) & 1);
            if (!direct) { // go left
                cend = min_it;
                if (cbegin == cend) {
                    if ((diff_m = static_cast<unsigned int>(cbegin - base)) <= UINT8_MAX) {
                        diff_a = diff_m;
                        diff_b = 1;
                    }
                    entry_as_ui = typename Node::CacheEntry{{diff_a, diff_b}}.as_uint16;
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimRight(node->diffs_.cbegin(), cbegin, cend, &min_val);
            } else { // go right
                build_cache_right:
                cbegin = min_it + 1;
                if (cbegin == cend) {
                    if ((diff_m = static_cast<unsigned int>(min_it /* cbegin - 1 */ - base)) <= UINT8_MAX) {
                        diff_a = diff_m;
                        diff_b = 1;
                    }
                    entry_as_ui = typename Node::CacheEntry{{diff_a, diff_b}}.as_uint16;
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimLeft(node->diffs_.cbegin(), cbegin, cend, &min_val);
            }

            if ((diff_m = static_cast<unsigned int>(cbegin - base)) <= UINT8_MAX &&
                (diff_n = cend - cbegin) <= UINT8_MAX) {
                diff_a = diff_m;
                diff_b = diff_n;
            }
            if (min_val - base_val >= 4) {
                entry_as_ui = typename Node::CacheEntry{{diff_a, diff_b}}.as_uint16;
                goto search;
            }
        }
#undef entry_as_ar
#undef entry_as_ui
#else
        const K_DIFF * cbegin = node->diffs_.cbegin();
        const K_DIFF * cend = &node->diffs_[size - 1];

        K_DIFF min_val;
        auto pyramid = node->pyramid_;
        const K_DIFF * min_it = cbegin + node->pyramid_.MinAt(cbegin, cend, &min_val);
        while (true) {
            assert(min_it == std::min_element(cbegin, cend) && *min_it == min_val);
            auto[diff_at, shift] = UnpackDiffAtAndShift(min_val);

            // left or right?
            uint8_t crit_byte = k.size() > diff_at
                                ? CharToUint8(k[diff_at])
                                : static_cast<uint8_t>(0);
            auto direct = ((crit_byte >> shift) & 1);
            if (!direct) { // go left
                cend = min_it;
                if (cbegin == cend) {
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimRight(node->diffs_.cbegin(), cbegin, cend, &min_val);
            } else { // go right
                cbegin = min_it + 1;
                if (cbegin == cend) {
                    return {min_it - node->diffs_.cbegin(), direct, size};
                }
                min_it = node->diffs_.cbegin() + pyramid.TrimLeft(node->diffs_.cbegin(), cbegin, cend, &min_val);
            }
        }
#endif
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    CombatInsert(const Slice & opponent, const Slice & k, KV_REP v,
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
                hint = nullptr;
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
                            cursor = OffsetToMemNode(kRootOffset);
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
                                 pyramid.TrimRight(cursor->diffs_.cbegin(), cbegin, cend, &exist_diff);
                    } else {
                        cbegin = min_it + 1;
                        if (cbegin == cend) {
                            insert_idx = min_it - cursor->diffs_.cbegin();
                            insert_direct = crit_direct;
                            break;
                        }
                        min_it = cursor->diffs_.cbegin() +
                                 pyramid.TrimLeft(cursor->diffs_.cbegin(), cbegin, cend, &exist_diff);
                    }
                }
            }

            const auto & rep = cursor->reps_[insert_idx + insert_direct];
            if (cursor->diffs_[insert_idx] > packed_diff || !IsPacked(rep)) {
                if (IsNodeFull(cursor)) {
                    try {
                        NodeSplit(cursor);
                    } catch (const AllocatorFullException &) {
                        size_t offset = reinterpret_cast<uintptr_t>(cursor) -
                                        reinterpret_cast<uintptr_t>(base_);
                        allocator_->Grow();
                        base_ = allocator_->Base();
                        cursor = OffsetToMemNode(offset);
                        NodeSplit(cursor);
                    }
                    continue;
                }
                NodeInsert(cursor, insert_idx, insert_direct,
                           direct, packed_diff, v, cursor_size);
                break;
            }
            cursor = OffsetToMemNode(Unpack(rep));
        }
        return true;
    }

#define add_gap(arr, idx, size) \
do { \
    auto idx__ = (idx); \
    auto size__ = (size); \
    memmove(&arr[idx__ + 1], &arr[idx__], sizeof(arr[0]) * (size__ - idx__)); \
} while (false)

#define del_gap(arr, idx, size) \
do { \
    auto idx__ = (idx); \
    auto size__ = (size); \
    auto indx__ = (idx__ + 1); \
    memmove(&arr[idx__], &arr[indx__], sizeof(arr[0]) * (size__ - indx__)); \
} while (false)

#define add_gaps(arr, idx, size, n) \
do { \
    auto idx__ = (idx); \
    auto size__ = (size); \
    auto n__ = (n); \
    memmove(&arr[idx__ + n__], &arr[idx__], sizeof(arr[0]) * (size__ - idx__)); \
} while (false)

#define del_gaps(arr, idx, size, n) \
do { \
    auto idx__ = (idx); \
    auto size__ = (size); \
    auto n__ = (n); \
    auto indx__ = (idx__ + n__); \
    memmove(&arr[idx__], &arr[indx__], sizeof(arr[0]) * (size__ - indx__)); \
} while (false)

#define cpy_part(dst, dst_idx, src, src_idx, n) \
do { \
    auto dst_idx__ = (dst_idx); \
    auto src_idx__ = (src_idx); \
    auto n__ = (n); \
    memcpy(&dst[dst_idx__], &src[src_idx__], sizeof(src[0]) * n__); \
} while (false)

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeSplit(Node * parent) {
        for (size_t i = 0; i < parent->reps_.size(); ++i) {
            const auto & rep = parent->reps_[i];
            if (IsPacked(rep)) {
                Node * child = OffsetToMemNode(Unpack(rep));
                if (!IsNodeFull(child)) {
                    size_t child_size = NodeSize(child);

                    // left child or right child?
                    if (i == 0 ||
                        (i != parent->reps_.size() - 1 && parent->diffs_[i - 1] < parent->diffs_[i])) { // left

                        // how long?
                        size_t j = i + 1;
                        for (; j < parent->diffs_.size(); ++j) {
                            if (parent->diffs_[j] < parent->diffs_[i]) {
                                break;
                            }
                        }

                        // enough space?
                        size_t range = j - i;
                        if (child_size + range <= child->reps_.size()) { // move to the tail
                            size_t child_diff_size = child_size - 1;
                            j = i + 1;

                            cpy_part(child->diffs_, child_diff_size, parent->diffs_, i, range);
                            cpy_part(child->reps_, child_size, parent->reps_, j, range);

                            del_gaps(parent->diffs_, i, parent->diffs_.size(), range);
                            del_gaps(parent->reps_, j, parent->reps_.size(), range);

                            parent->size_ -= range;
                            child->size_ += range;
                            assert(NodeSize(parent) == parent->reps_.size() - range);
                            assert(NodeSize(child) == child_size + range);
                            NodeBuild(parent, i);
                            NodeBuild(child, child_diff_size);
                            return;
                        }
                    } else { // right

                        size_t j = i - 1;
                        while (j != 0) {
                            if (parent->diffs_[j - 1] < parent->diffs_[i - 1]) {
                                break;
                            }
                            --j;
                        }

                        size_t range = i - j;
                        if (child_size + range <= child->reps_.size()) { // move to the head
                            add_gaps(child->diffs_, 0, child_size - 1, range);
                            add_gaps(child->reps_, 0, child_size, range);

                            cpy_part(child->diffs_, 0, parent->diffs_, j, range);
                            cpy_part(child->reps_, 0, parent->reps_, j, range);

                            del_gaps(parent->diffs_, j, parent->diffs_.size(), range);
                            del_gaps(parent->reps_, j, parent->reps_.size(), range);

                            parent->size_ -= range;
                            child->size_ += range;
                            assert(NodeSize(parent) == parent->reps_.size() - range);
                            assert(NodeSize(child) == child_size + range);
                            NodeBuild(parent, j);
                            NodeBuild(child);
                            return;
                        }
                    }
                }
            }
        }

        size_t offset = allocator_->AllocatePage(); // may throw AllocatorFullException
        Node * child = new(OffsetToMemNode(offset)) Node();

        // find nearly half
        const K_DIFF * cbegin = parent->diffs_.cbegin();
        const K_DIFF * cend = parent->diffs_.cend();

        auto pyramid = parent->pyramid_;
        const K_DIFF * min_it = cbegin + parent->pyramid_.MinAt(cbegin, cend);
        while (true) {
            assert(min_it == std::min_element(cbegin, cend));
            if (min_it - cbegin <= cend - min_it) { // go right
                cbegin = min_it + 1;
                if (static_cast<size_t>(cend - cbegin) < parent->diffs_.size() / 2) {
                    break;
                }
                min_it = parent->diffs_.cbegin() +
                         pyramid.TrimLeft(parent->diffs_.cbegin(), cbegin, cend);
            } else { // go left
                cend = min_it;
                if (static_cast<size_t>(cend - cbegin) < parent->diffs_.size() / 2) {
                    break;
                }
                min_it = parent->diffs_.cbegin() +
                         pyramid.TrimRight(parent->diffs_.cbegin(), cbegin, cend);
            }
        }

        size_t item_num = cend - cbegin;
        size_t child_size = item_num + 1;
        size_t nth = cbegin - parent->diffs_.cbegin();

        cpy_part(child->diffs_, 0, parent->diffs_, nth, item_num);
        cpy_part(child->reps_, 0, parent->reps_, nth, child_size);

        del_gaps(parent->diffs_, nth, parent->diffs_.size(), item_num);
        del_gaps(parent->reps_, nth + 1, parent->reps_.size(), item_num);
        parent->reps_[nth] = Pack(offset);

        child->size_ = static_cast<uint32_t>(child_size);
        parent->size_ -= item_num;
        NodeBuild(parent, nth);
        NodeBuild(child);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeMerge(Node * parent, size_t idx, bool direct, size_t parent_size,
              Node * child, size_t child_size) {
        idx += static_cast<size_t>(direct);
        size_t offset = Unpack(parent->reps_[idx]);
        size_t child_diff_size = child_size - 1;

        add_gaps(parent->diffs_, idx, parent_size - 1, child_diff_size);
        add_gaps(parent->reps_, idx + 1, parent_size, child_diff_size);

        cpy_part(parent->diffs_, idx, child->diffs_, 0, child_diff_size);
        cpy_part(parent->reps_, idx, child->reps_, 0, child_size);

        allocator_->FreePage(offset);
        parent->size_ += child_diff_size;
        NodeBuild(parent, idx);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeCompact(Node * node) {
        for (size_t i = 0; !IsNodeFull(node) && i < NodeSize(node); ++i) {
            restart:
            const auto & rep = node->reps_[i];
            if (IsPacked(rep)) {
                Node * child = OffsetToMemNode(Unpack(rep));
                size_t child_size = NodeSize(child);
                size_t node_size = NodeSize(node);

                if (node->reps_.size() - node_size + 1 >= child_size) {
                    NodeMerge(node, i, false, node_size,
                              child, child_size);
                    goto restart;
                }

                const K_DIFF * cbegin = child->diffs_.cbegin();
                const K_DIFF * cend = &child->diffs_[child_size - 1];
                const K_DIFF * min_it = cbegin + child->pyramid_.MinAt(cbegin, cend);
                assert(min_it == std::min_element(cbegin, cend));

                if (min_it - cbegin < cend - min_it) { // go left
                    cend = min_it + 1;
                    size_t item_num = cend - cbegin;
                    if (item_num + node_size <= node->reps_.size()) {
                        add_gaps(node->diffs_, i, node_size - 1, item_num);
                        add_gaps(node->reps_, i, node_size, item_num);

                        cpy_part(node->diffs_, i, child->diffs_, 0, item_num);
                        cpy_part(node->reps_, i, child->reps_, 0, item_num);

                        del_gaps(child->diffs_, 0, child_size - 1, item_num);
                        del_gaps(child->reps_, 0, child_size, item_num);

                        node->size_ += item_num;
                        child->size_ -= item_num;
                        NodeBuild(node, i);
                        NodeBuild(child);
                        goto restart;
                    }
                } else { // go right
                    cbegin = min_it;
                    size_t item_num = cend - cbegin;
                    if (item_num + node_size <= node->reps_.size()) {
                        size_t nth = cbegin - child->diffs_.cbegin();
                        size_t j = i + 1;

                        add_gaps(node->diffs_, i, node_size - 1, item_num);
                        add_gaps(node->reps_, j, node_size, item_num);

                        cpy_part(node->diffs_, i, child->diffs_, nth, item_num);
                        cpy_part(node->reps_, j, child->reps_, nth + 1, item_num);

                        node->size_ += item_num;
                        child->size_ -= item_num;
                        NodeBuild(node, i);
                        NodeBuild(child, nth);
                        goto restart;
                    }
                }
            }
        }

        for (size_t i = 0; i < NodeSize(node); ++i) {
            const auto & rep = node->reps_[i];
            if (IsPacked(rep)) {
                NodeCompact(OffsetToMemNode(Unpack(rep)));
            }
        }
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
        node->size_ = size + 1;
        NodeBuild(node, insert_idx);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeRemove(Node * node, size_t idx, bool direct, size_t size) {
        assert(size >= 1);
        del_gap(node->reps_, idx + direct, size);
        node->size_ = --size;
        if (SGT_LIKELY(size > 0)) {
            del_gap(node->diffs_, idx, size);
            NodeBuild(node, idx);
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeBuild(Node * node, size_t rebuild_idx) {
#ifndef SGT_NO_DENSE_INPUT_CACHE
        node->cache_ = {};
#endif
        node->pyramid_.Build(node->diffs_.data(), node->diffs_.data() + NodeSize(node) - 1, rebuild_idx);
    }

#undef add_gap
#undef del_gap
#undef add_gaps
#undef del_gaps
#undef cpy_part
}

#endif //SIG_TREE_SIG_TREE_IMPL_H
