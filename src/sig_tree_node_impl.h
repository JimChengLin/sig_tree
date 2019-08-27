#pragma once
#ifndef SIG_TREE_SIG_TREE_NODE_IMPL_H
#define SIG_TREE_SIG_TREE_NODE_IMPL_H

#if __has_include(<smmintrin.h>)

#include <smmintrin.h>

namespace sgt {
    constexpr bool kHasMinpos = true;
}
#else
namespace sgt {
    constexpr bool kHasMinpos = false;
}
#endif

#include "sig_tree.h"

namespace sgt {
    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    size_t SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeSize(const Node * node) {
        return node->size_;
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    bool SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    IsNodeFull(const Node * node) {
        return node->size_ == kNodeRepRank;
    }

    template<typename T>
    const T * SmartMinElem8(const T * from, const T * to, T * min_val) {
        if constexpr (std::is_same<T, uint16_t>::value && kHasMinpos) {
            __m128i vec = _mm_loadu_si128(reinterpret_cast<const __m128i *>(from));

            size_t size = to - from;
            if (size != 8) {
                assert(size < 8);

                static constexpr auto masks = []() {
                    std::array<std::array<uint64_t, 2>, 8> arr{};
                    arr[1][0] = UINT16_MAX;
                    arr[2][0] = (arr[1][0] << 16) | UINT16_MAX;
                    arr[3][0] = (arr[2][0] << 16) | UINT16_MAX;
                    arr[4][0] = (arr[3][0] << 16) | UINT16_MAX;
                    for (size_t i = 5; i < 8; ++i) { arr[i][0] = UINT64_MAX; }
                    arr[5][1] = UINT16_MAX;
                    arr[6][1] = (arr[5][1] << 16) | UINT16_MAX;
                    arr[7][1] = (arr[6][1] << 16) | UINT16_MAX;
                    for (size_t i = 0; i < 8; ++i) { arr[i] = {~arr[i][0], ~arr[i][1]}; }
                    return arr;
                }();

                vec = _mm_or_si128(vec, _mm_loadu_si128(reinterpret_cast<const __m128i *>(&masks[size])));
            }

            vec = _mm_minpos_epu16(vec);
            if (min_val != nullptr) { *min_val = static_cast<T>(_mm_extract_epi16(vec, 0)); }
            return from + _mm_extract_epi8(vec, 2);
        } else {
            const T * min_it = std::min_element(from, to);
            if (min_val != nullptr) { *min_val = *min_it; }
            return min_it;
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<size_t RANK>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeTpl<RANK>::Pyramid::Build(const K_DIFF * from, const K_DIFF * to, size_t rebuild_idx) {
        size_t size = to - from;
        if (size <= 8) {
            return;
        } else if (size == 9) {
            rebuild_idx = 0;
        }

        size_t level = 0;
        while (true) {
            const size_t q = size / 8;
            const size_t r = size % 8;
            K_DIFF * val_from = vals_.begin() + kAbsOffsets[level];
            uint8_t * idx_from = idxes_.begin() + kAbsOffsets[level++];
            const K_DIFF * next_from = val_from;

            if (rebuild_idx > 0) {
                rebuild_idx /= 8;
                val_from += rebuild_idx;
                idx_from += rebuild_idx;
                from += (8 * rebuild_idx);
            }

            while (to - from >= 8) {
                K_DIFF val;
                uint8_t idx;
                if constexpr (std::is_same<K_DIFF, uint16_t>::value && kHasMinpos) {
                    __m128i vec = _mm_loadu_si128(reinterpret_cast<const __m128i *>(from));
                    vec = _mm_minpos_epu16(vec);
                    val = static_cast<K_DIFF>(_mm_extract_epi16(vec, 0));
                    idx = static_cast<uint8_t>(_mm_extract_epi8(vec, 2));
                } else {
                    const K_DIFF * min_elem = std::min_element(from, from + 8);
                    val = *min_elem;
                    idx = static_cast<uint8_t>(min_elem - from);
                }

                (*val_from++) = val;
                (*idx_from++) = idx;
                from += 8;
            }

            if (r != 0) {
                size = q + 1;
                const K_DIFF * min_elem = SmartMinElem8(from, to, val_from);
                (*idx_from) = static_cast<uint8_t>(min_elem - from);
            } else {
                size = q;
            }

            if (size == 1) {
                break;
            }
            from = next_from;
            to = from + size;
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<size_t RANK>
    size_t SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeTpl<RANK>::Pyramid::MinAt(const K_DIFF * from, const K_DIFF * to,
                                  K_DIFF * min_val) const {
        size_t size = to - from;
        if (size <= 8) {
            static_assert(!(std::is_same<K_DIFF, uint16_t>::value && kHasMinpos)
                          || sizeof(idxes_) >= sizeof(uint16_t) * 8);
            const K_DIFF * min_it = SmartMinElem8(from, to, min_val);
            return min_it - from;
        }
        return CalcOffset(PyramidHeight(size) - 1, 0, min_val);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<size_t RANK>
    size_t SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeTpl<RANK>::Pyramid::TrimLeft(const K_DIFF * cbegin, const K_DIFF * from, const K_DIFF * to,
                                     K_DIFF * min_val) {
        size_t pos = from - cbegin;
        size_t end_pos = to - cbegin;
        assert(end_pos >= pos + 1);
        if (end_pos - pos <= 8) {
            const K_DIFF * min_it = SmartMinElem8(from, to, min_val);
            return min_it - cbegin;
        }

        bool accumulator = true;
        size_t level = 0;
        do {
            const size_t q = pos / 8;
            const size_t r = pos % 8;
            pos = q;
            end_pos = end_pos / 8 + static_cast<size_t>(end_pos % 8 != 0);

            const size_t offset = kAbsOffsets[level++];
            uint8_t & upper_idx = idxes_[offset + pos];
            if ((accumulator = (static_cast<size_t>(upper_idx + accumulator) > r))) {
                cbegin = vals_.cbegin() + offset;
                from = cbegin + pos;
                to = cbegin + end_pos;
            } else {
                K_DIFF val;
                const K_DIFF * min_elem = SmartMinElem8(from, std::min(from + (8 - r), to), &val);
                const size_t idx = (min_elem - from) + r;

                cbegin = vals_.cbegin() + offset;
                from = cbegin + pos;
                to = cbegin + end_pos;

                *const_cast<K_DIFF *>(from) = val;
                upper_idx = static_cast<uint8_t>(idx);
            }
        } while (end_pos - pos > 1);
        return CalcOffset(level - 1, pos, min_val);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<size_t RANK>
    size_t SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeTpl<RANK>::Pyramid::TrimRight(const K_DIFF * cbegin, const K_DIFF * from, const K_DIFF * to,
                                      K_DIFF * min_val) {
        size_t pos = from - cbegin;
        size_t end_pos = to - cbegin;
        assert(end_pos >= pos + 1);
        if (end_pos - pos <= 8) {
            const K_DIFF * min_it = SmartMinElem8(from, to, min_val);
            return min_it - cbegin;
        }

        bool accumulator = true;
        size_t level = 0;
        do {
            size_t q = end_pos / 8;
            size_t r = end_pos % 8;
            if (r == 0) {
                --q;
                r = 8;
            }
            pos /= 8;
            end_pos = q + 1;

            const size_t offset = kAbsOffsets[level++];
            uint8_t & upper_idx = idxes_[offset + q /* end_pos - 1 */];
            if ((accumulator = (upper_idx < r - 1 + accumulator))) {
                cbegin = vals_.cbegin() + offset;
                from = cbegin + pos;
                to = cbegin + end_pos;
            } else {
                K_DIFF val;
                const K_DIFF * start = to - r;
                const K_DIFF * min_elem = SmartMinElem8(std::max(from, start), to, &val);
                const size_t idx = min_elem - start;

                cbegin = vals_.cbegin() + offset;
                from = cbegin + pos;
                to = cbegin + end_pos;

                *const_cast<K_DIFF *>(to - 1) = val;
                upper_idx = static_cast<uint8_t>(idx);
            }
        } while (end_pos - pos > 1);
        return CalcOffset(level - 1, pos, min_val);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    template<size_t RANK>
    size_t SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    NodeTpl<RANK>::Pyramid::CalcOffset(size_t level, size_t index, K_DIFF * min_val) const {
        size_t i = kAbsOffsets[level] + index;
        if (min_val != nullptr) { *min_val = vals_[i]; }

        size_t r = idxes_[i];
        if constexpr (PyramidHeight(kNodeRank) == 3) {
            assert(level < 3);
            if (level == 2) {
                index = index * 8 + r;
                r = idxes_[kAbsOffsets[1] + index];
            }
            index = index * 8 + r;
            r = idxes_[kAbsOffsets[0] + index];
        } else {
            do {
                index = index * 8 + r;
                r = idxes_[kAbsOffsets[--level] + index];
            } while (level != 0);
        }
        return index * 8 + r;
    }
}

#endif //SIG_TREE_SIG_TREE_NODE_IMPL_H
