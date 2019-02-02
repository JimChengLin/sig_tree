#pragma once
#ifndef SIG_TREE_SIG_TREE_H
#define SIG_TREE_SIG_TREE_H

/*
 * 签名树
 * 作者: 左吉吉
 * 发布协议: MIT
 * 完成时间: 2018春
 *
 * SGT 无法区分 "abc\0\0" 与 "abc\0"
 * 可选的解决方案: 结尾全局唯一符号, C 式字符串等
 */

#include <array>
#include <climits>

#include "allocator.h"
#include "kv_trans_trait.h"
#include "page_size.h"

namespace sgt {
    template<
            typename KV_TRANS, // KV_REP => K, V
            typename K_DIFF = uint16_t,
            typename KV_REP = uint64_t>
    class SignatureTreeTpl {
        static_assert(is_kv_trans<KV_TRANS>::value);
        static_assert(!std::numeric_limits<K_DIFF>::is_signed);

    public:
        class Helper {
        public:
            Helper() = default;

            virtual ~Helper() = default;

        public:
            virtual KV_REP Add(const Slice & k, const Slice & v) = 0;

            virtual void Del(KV_TRANS & trans) = 0;

            virtual KV_REP Pack(size_t offset) const = 0;

            virtual size_t Unpack(const KV_REP & rep) const = 0;

            virtual bool IsPacked(const KV_REP & rep) const = 0;

            virtual KV_TRANS Trans(const KV_REP & rep) const = 0;
        };

    private:
        Helper * const helper_;
        Allocator * const allocator_;
        const size_t kRootOffset;

    public:
        SignatureTreeTpl(Helper * helper, Allocator * allocator);

        SignatureTreeTpl(Helper * helper, Allocator * allocator, size_t root_offset)
                : helper_(helper),
                  allocator_(allocator),
                  kRootOffset(root_offset) {}

        SignatureTreeTpl(const SignatureTreeTpl &) = delete;

        SignatureTreeTpl & operator=(const SignatureTreeTpl &) = delete;

    public:
        bool Get(const Slice & k, std::string * v) const;

        size_t Size() const;

        size_t RootOffset() const { return kRootOffset; }

        // bool(* visitor)(const KV_REP & rep)
        template<bool BACKWARD, typename VISITOR>
        void Visit(const Slice & target, VISITOR && visitor) const {
            VisitGenericImpl<const SignatureTreeTpl *, BACKWARD>(this, target, std::forward<VISITOR>(visitor));
        }

        // std::pair<bool, bool>(* visitor)(const KV_REP & rep)
        template<bool BACKWARD, typename VISITOR>
        void VisitDel(const Slice & target, VISITOR && visitor) {
            VisitGenericImpl<SignatureTreeTpl *, BACKWARD>(this, target, std::forward<VISITOR>(visitor));
        }

        // bool(* if_dup_callback)(KV_TRANS & trans, KV_REP & rep)
        template<typename IF_DUP_CALLBACK  = std::false_type>
        bool Add(const Slice & k, const Slice & v,
                 IF_DUP_CALLBACK && if_dup_callback = {});

        bool Del(const Slice & k);

        void Compact();

        void Rebuild(SignatureTreeTpl * dst);

    private:
        enum {
            kPyramidBrickLength = 8
        };

        inline static constexpr size_t PyramidBrickNum(size_t rank) {
            size_t num = 0;
            do {
                rank = (rank + kPyramidBrickLength - 1) / kPyramidBrickLength;
                num += rank;
            } while (rank > 1);
            return num;
        }

        inline static constexpr size_t PyramidHeight(size_t rank) {
            size_t height = 0;
            do {
                rank = (rank + kPyramidBrickLength - 1) / kPyramidBrickLength;
                ++height;
            } while (rank > 1);
            return height;
        }

        template<size_t RANK>
        struct NodeTpl {
            struct Pyramid {
                enum {
                    kBrickNum = PyramidBrickNum(RANK),
                    kHeight = PyramidHeight(RANK)
                };

                std::array<K_DIFF, kBrickNum> vals_;
                std::array<uint8_t, kBrickNum> idxes_;

                static constexpr auto kAbsOffsets = []() {
                    std::array<size_t, kHeight> arr{};
                    size_t i = 0;
                    size_t offset = 0;
                    size_t rank = RANK;
                    do {
                        arr[i++] = offset;
                        rank = (rank + kPyramidBrickLength - 1) / kPyramidBrickLength;
                        offset += rank;
                    } while (rank > 1);
                    return arr;
                }();

                void Build(const K_DIFF * from, const K_DIFF * to, size_t rebuild_idx);

                size_t MinAt(const K_DIFF * from, const K_DIFF * to) const;

                size_t TrimLeft(const K_DIFF * cbegin, const K_DIFF * from, const K_DIFF * to);

                size_t TrimRight(const K_DIFF * cbegin, const K_DIFF * from, const K_DIFF * to);

                size_t CalcOffset(size_t level, size_t index) const;
            };

            std::array<KV_REP, RANK + 1> reps_;
            std::array<K_DIFF, RANK> diffs_;
            uint32_t size_ = 0;
            Pyramid pyramid_;
        };

        template<size_t RANK = 1, bool UP = true>
        struct NodeRank {
            enum {
                kSize = sizeof(NodeTpl<RANK>),
                value = kSize > kPageSize
                        ? static_cast<int>(NodeRank<RANK - 1, false>::value)
                        : static_cast<int>(NodeRank<RANK + 1, !(kSize > kPageSize)>::value)
            };
        };

        template<size_t RANK>
        struct NodeRank<RANK, false> {
            enum {
                kSize = sizeof(NodeTpl<RANK>),
                value = RANK
            };
        };

        typedef NodeTpl<NodeRank<>::value> Node;
        static_assert(std::is_standard_layout<Node>::value &&
                      std::is_trivially_copyable<Node>::value);

    private:
        Node * OffsetToMemNode(size_t offset) const {
            return reinterpret_cast<Node *>(reinterpret_cast<char *>(allocator_->Base()) + offset);
        }

        static std::tuple<size_t /* idx */, bool /* direct */, size_t /* size */>
        FindBestMatch(const Node * node, const Slice & k);

        bool CombatInsert(const Slice & opponent, const Slice & k, const Slice & v,
                          Node * hint, size_t hint_idx, bool hint_direct);

        void NodeSplit(Node * parent);

        void NodeMerge(Node * parent, size_t idx, bool direct, size_t parent_size,
                       Node * child, size_t child_size);

        void NodeCompact(Node * node);

        static void NodeInsert(Node * node, size_t insert_idx, bool insert_direct,
                               bool direct, K_DIFF diff, const KV_REP & rep, size_t size);

        static void NodeRemove(Node * node, size_t idx, bool direct, size_t size);

        static void NodeBuild(Node * node, size_t rebuild_idx = 0);

        static size_t NodeSize(const Node * node);

        static bool IsNodeFull(const Node * node);

        static K_DIFF PackDiffAtAndShift(K_DIFF diff_at, uint8_t shift) {
            return (diff_at << 3) | ((~shift) & 0b111);
        }

        static std::pair<K_DIFF, uint8_t>
        UnpackDiffAtAndShift(K_DIFF packed_diff) {
            return {packed_diff >> 3, (~packed_diff) & 0b111};
        }

        template<typename T, bool BACKWARD, typename VISITOR>
        static void VisitGenericImpl(T self, const Slice & target, VISITOR && visitor);

    public:
        enum {
            kNodeRank = NodeRank<>::value,
            kNodeRepRank = kNodeRank + 1,
            kMaxKeyLength = std::numeric_limits<K_DIFF>::max() >> 3,
            kForward = false,
            kBackward = true,
        };
    };
}

#endif //SIG_TREE_SIG_TREE_H
