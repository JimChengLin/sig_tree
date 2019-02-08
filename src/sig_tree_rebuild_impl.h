#pragma once
#ifndef SIG_TREE_SIG_TREE_REBUILD_IMPL_H
#define SIG_TREE_SIG_TREE_REBUILD_IMPL_H

#include "sig_tree.h"

namespace sgt {
    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    Rebuild(SignatureTreeTpl * dst) const {
        RebuildPageToNode(RebuildHeadNode(OffsetToMemNode(kRootOffset), dst),
                          dst->OffsetToMemNode(dst->kRootOffset));
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    typename SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::Page
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    RebuildHeadNode(const Node * node, SignatureTreeTpl * dst) const {
        size_t size = NodeSize(node);
        if (SGT_UNLIKELY(size <= 1)) {
            return {{},
                    {node->reps_.data(), node->reps_.data() + size}};
        }

        const K_DIFF * cbegin = node->diffs_.cbegin();
        const K_DIFF * cend = &node->diffs_[size - 1];

        auto pyramid = node->pyramid_;
        const K_DIFF * min_it = cbegin + pyramid.MinAt(cbegin, cend);

        Page l = RebuildInternalNode(node, cbegin, cend, min_it, pyramid, false, dst);
        Page r = RebuildInternalNode(node, cbegin, cend, min_it, pyramid, true, dst);
        return RebuildLRPagesToTree(std::move(l), std::move(r), *min_it, dst);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    typename SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::Page
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    RebuildInternalNode(const Node * node,
                        const K_DIFF * cbegin, const K_DIFF * cend, const K_DIFF * min_it,
                        typename Node::Pyramid & pyramid, bool direct, SignatureTreeTpl * dst) const {
        assert(min_it == std::min_element(cbegin, cend));
        if (!direct) {  // go left
            cend = min_it;
            if (cbegin == cend) {
                const auto & rep = node->reps_[cend - node->diffs_.cbegin()];
                return helper_->IsPacked(rep) ? RebuildHeadNode(OffsetToMemNode(helper_->Unpack(rep)), dst)
                                              : Page{{},
                                                     {{rep}}};
            }
            min_it = node->diffs_.cbegin() + pyramid.TrimRight(node->diffs_.cbegin(), cbegin, cend);
        } else {  // go right
            cbegin = min_it + 1;
            if (cbegin == cend) {
                const auto & rep = node->reps_[cend - node->diffs_.cbegin()];
                return helper_->IsPacked(rep) ? RebuildHeadNode(OffsetToMemNode(helper_->Unpack(rep)), dst)
                                              : Page{{},
                                                     {{rep}}};
            }
            min_it = node->diffs_.cbegin() + pyramid.TrimLeft(node->diffs_.cbegin(), cbegin, cend);
        }

        Page l = RebuildInternalNode(node, cbegin, cend, min_it, pyramid, false, dst);
        Page r = RebuildInternalNode(node, cbegin, cend, min_it, pyramid, true, dst);
        return RebuildLRPagesToTree(std::move(l), std::move(r), *min_it, dst);
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    typename SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::Page
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    RebuildLRPagesToTree(Page && l, Page && r, K_DIFF diff, SignatureTreeTpl * dst) {
        if (l.reps.size() + r.reps.size() <= kNodeRepRank) {
            l.diffs.emplace_back(diff);
            l.diffs.insert(l.diffs.end(), r.diffs.begin(), r.diffs.end());
            l.reps.insert(l.reps.end(), r.reps.begin(), r.reps.end());
            return l;
        } else {
            if (l.reps.size() <= r.reps.size()) {
                l.diffs.emplace_back(diff);
                l.reps.emplace_back(dst->helper_->Pack(RebuildPageToTree(r, dst)));
                return l;
            } else {
                r.diffs.insert(r.diffs.begin(), diff);
                r.reps.insert(r.reps.begin(), dst->helper_->Pack(RebuildPageToTree(l, dst)));
                return r;
            }
        }
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    size_t SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    RebuildPageToTree(const Page & page, SignatureTreeTpl * dst) {
        size_t offset;
        try {
            offset = dst->allocator_->AllocatePage();
        } catch (const AllocatorFullException &) {
            dst->allocator_->Grow();
            offset = dst->allocator_->AllocatePage();
        }
        Node * node = new(dst->OffsetToMemNode(offset)) Node();
        RebuildPageToNode(page, node);
        return offset;
    }

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    void SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::
    RebuildPageToNode(const Page & page, Node * node) {
        std::copy(page.reps.cbegin(), page.reps.cend(), node->reps_.begin());
        std::copy(page.diffs.cbegin(), page.diffs.cend(), node->diffs_.begin());
        node->size_ = static_cast<uint32_t>(page.reps.size());
        NodeBuild(node);
    }
}

#endif //SIG_TREE_SIG_TREE_REBUILD_IMPL_H