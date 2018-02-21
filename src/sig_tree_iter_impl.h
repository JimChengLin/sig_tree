#pragma once
#ifndef SIG_TREE_SIG_TREE_ITER_IMPL_H
#define SIG_TREE_SIG_TREE_ITER_IMPL_H

/*
 * 在使用 IteratorImpl 时, 不允许同时修改 SGT
 * 如果 SGT 发生变动, 须重新调用 Seek. SGT 的 Seek 非常快!
 *
 * Seek 语义是 PrefixSeek 而非 std::lower_bound
 */

#include <optional>

#include "iterator.h"
#include "sig_tree.h"

namespace sgt {
    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    class SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::IteratorImpl : public Iterator {
    private:
        const SignatureTreeTpl * tree_;
        std::vector<std::pair<size_t /* node offset */, ssize_t /* KV_REP idx */> > que_;
        // lazy eval
        mutable std::string out_;
        mutable std::optional<KV_TRANS> trans_;

    public:
        explicit IteratorImpl(const SignatureTreeTpl * tree) : tree_(tree) {}

        ~IteratorImpl() override = default;

    public:
        bool Valid() const override {
            return !que_.empty();
        }

        void SeekToFirst() override { // leftmost node
            que_.clear();
            trans_.reset();

            size_t offset = tree_->kRootOffset;
            const Node * cursor = tree_->OffsetToMemNode(offset);
            if (tree_->NodeSize(cursor) == 0) {
                return;
            }

            while (true) {
                que_.emplace_back(offset, 0);
                const auto & rep = cursor->reps_[0];
                if (tree_->helper_->IsPacked(rep)) {
                    offset = tree_->helper_->Unpack(rep);
                    cursor = tree_->OffsetToMemNode(offset);
                } else {
                    break;
                }
            }
        }

        void SeekToLast() override { // rightmost node
            que_.clear();
            trans_.reset();

            size_t offset = tree_->kRootOffset;
            const Node * cursor = tree_->OffsetToMemNode(offset);
            size_t size = tree_->NodeSize(cursor);
            if (size == 0) {
                return;
            }

            while (true) {
                que_.emplace_back(offset, --size);
                const auto & rep = cursor->reps_[size];
                if (tree_->helper_->IsPacked(rep)) {
                    offset = tree_->helper_->Unpack(rep);
                    cursor = tree_->OffsetToMemNode(offset);
                    size = tree_->NodeSize(cursor);
                } else {
                    break;
                }
            }
        }

        void Seek(const Slice & target) override {
            que_.clear();
            trans_.reset();

            size_t offset = tree_->kRootOffset;
            const Node * cursor = tree_->OffsetToMemNode(offset);
            if (tree_->NodeSize(cursor) == 0) {
                return;
            }

            while (true) {
                size_t idx;
                bool direct;
                std::tie(idx, direct, std::ignore) = tree_->FindBestMatch(cursor, target);

                idx += direct;
                que_.emplace_back(offset, idx);
                const auto & rep = cursor->reps_[idx];
                if (tree_->helper_->IsPacked(rep)) {
                    offset = tree_->helper_->Unpack(rep);
                    cursor = tree_->OffsetToMemNode(offset);
                } else {
                    break;
                }
            }
        }

        void Next() override {
            trans_.reset();
            while (!que_.empty()) {
                size_t node_offset;
                ssize_t rep_idx;
                std::tie(node_offset, rep_idx) = que_.back();

                const Node * cursor = tree_->OffsetToMemNode(node_offset);
                if (rep_idx + 1 == cursor->reps_.size()
                    || cursor->reps_[rep_idx + 1] == tree_->kNullRep) {
                    que_.pop_back();
                } else {
                    rep_idx = ++que_.back().second;
                    const auto & rep = cursor->reps_[rep_idx];
                    if (tree_->helper_->IsPacked(rep)) {
                        que_.emplace_back(tree_->helper_->Unpack(rep), -1);
                    } else {
                        break;
                    }
                }
            }
        }

        void Prev() override {
            trans_.reset();
            while (!que_.empty()) {
                size_t node_offset;
                ssize_t rep_idx;
                std::tie(node_offset, rep_idx) = que_.back();

                if (rep_idx == 0) {
                    que_.pop_back();
                } else {
                    rep_idx = --que_.back().second;
                    const Node * cursor = tree_->OffsetToMemNode(node_offset);
                    const auto & rep = cursor->reps_[rep_idx];
                    if (tree_->helper_->IsPacked(rep)) {
                        node_offset = tree_->helper_->Unpack(rep);
                        cursor = tree_->OffsetToMemNode(node_offset);
                        que_.emplace_back(node_offset, tree_->NodeSize(cursor));
                    } else {
                        break;
                    }
                }
            }
        }

        Slice Key() const override {
            if (trans_.has_value()) {
                return trans_.value().Key();
            } else {
                const Node * cursor = tree_->OffsetToMemNode(que_.back().first);
                trans_.emplace(tree_->helper_->Trans(cursor->reps_[que_.back().second]));
                trans_.value().Get(trans_.value().Key(), &out_);
                return Key();
            }
        }

        Slice Value() const override {
            if (trans_.has_value()) {
                return out_;
            } else {
                Key();
                return Value();
            }
        }
    };

    template<typename KV_TRANS, typename K_DIFF, typename KV_REP>
    typename SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::IteratorImpl
    SignatureTreeTpl<KV_TRANS, K_DIFF, KV_REP>::GetIterator() const {
        return IteratorImpl(static_cast<const SignatureTreeTpl *>(this));
    }
}

#endif //SIG_TREE_SIG_TREE_ITER_IMPL_H
