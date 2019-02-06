#include <iostream>
#include <random>
#include <set>
#include <unordered_set>

#include "../src/sig_tree.h"
#include "../src/sig_tree_impl.h"
#include "../src/sig_tree_node_impl.h"
#include "../src/sig_tree_rebuild.h"
#include "../src/sig_tree_visit_impl.h"

namespace sgt {
    namespace sig_tree_test {
        /*
         * K = uint32_t
         * V = uint32_t
         * little-endian REP : uint64_t = (V << 32) | K
         *
         * NullRep = UINT64_MAX
         * if REP % 2 = 0, then REP is packed
         */

        class KVTrans {
        private:
            const uint64_t rep_;

        public:
            explicit KVTrans(uint64_t rep) : rep_(rep) {}

        public:
            bool operator==(const Slice & k) const {
                return memcmp(&rep_, k.data(), sizeof(uint32_t)) == 0;
            }

            Slice Key() const {
                return {reinterpret_cast<const char *>(&rep_), sizeof(uint32_t)};
            }

            bool Get(const Slice & k, std::string * v) const {
                if (*this == k) {
                    if (v != nullptr) {
                        v->assign(reinterpret_cast<const char *>(&rep_) + sizeof(uint32_t),
                                  reinterpret_cast<const char *>(&rep_) + sizeof(uint64_t));
                    }
                    return true;
                }
                return false;
            }
        };

        class Helper : public SignatureTreeTpl<KVTrans>::Helper {
        public:
            ~Helper() override = default;

        public:
            uint64_t Add(const Slice & k, const Slice & v) override {
                assert(k.size() == sizeof(uint32_t) && v.size() == sizeof(uint32_t));
                uint32_t ki;
                uint32_t vi;
                memcpy(&ki, k.data(), sizeof(ki));
                memcpy(&vi, v.data(), sizeof(vi));
                assert(ki % 2 == 1);
                return (static_cast<uint64_t>(vi) << 32) | ki;
            }

            void Del(KVTrans & trans) override {
            }

            uint64_t Pack(size_t offset) const override {
                assert(offset % 2 == 0);
                return offset;
            }

            size_t Unpack(const uint64_t & rep) const override {
                return rep;
            }

            bool IsPacked(const uint64_t & rep) const override {
                return rep % 2 == 0;
            }

            KVTrans Trans(const uint64_t & rep) const override {
                return KVTrans(rep);
            }
        };

        class AllocatorImpl : public Allocator {
        public:
            std::unordered_set<uintptr_t> records_;

        public:
            ~AllocatorImpl() override {
                for (uintptr_t record:records_) {
                    free(reinterpret_cast<void *>(record));
                }
            }

        public:
            void * Base() override {
                return nullptr;
            }

            size_t AllocatePage() override {
                auto page = reinterpret_cast<uintptr_t>(malloc(kPageSize));
                records_.emplace(page);
                return page;
            }

            void FreePage(size_t offset) override {
                auto it = records_.find(offset);
                free(reinterpret_cast<void *>(*it));
                records_.erase(it);
            }

            void Grow() override {
            }
        };

        void Run() {
            constexpr unsigned int kTestTimes = 10000;

            Helper helper;
            AllocatorImpl allocator;
            SignatureTreeTpl<KVTrans> tree(&helper, &allocator);

            struct cmp {
                bool operator()(uint32_t a, uint32_t b) const {
                    return memcmp(&a, &b, sizeof(uint32_t)) < 0;
                }
            };
            std::set<uint32_t, cmp> set;

            auto seed = std::random_device()();
            std::cout << "sig_tree_test_seed: " << seed << std::endl;

            std::default_random_engine engine(seed);
            auto dist = std::uniform_int_distribution<uint32_t>(0, UINT32_MAX >> 1);
            for (size_t i = 0; i < kTestTimes; ++i) {
                auto v = dist(engine);
                v += (v % 2 == 0);

                set.emplace(v);
                Slice s(reinterpret_cast<char *>(&v), sizeof(v));
                tree.Add(s, s);
                assert(tree.Size() == set.size());
            }
            tree.Compact();

            {
                auto it = set.cbegin();
                tree.Visit<tree.kForward>("", [&it](const uint64_t & rep) {
                    uint32_t v = *it++;
                    return v == (rep >> 32);
                });
                assert(it == set.cend());
            }
            {
                auto it = set.crbegin();
                tree.Visit<tree.kBackward>("", [&it](const uint64_t & rep) {
                    uint32_t v = *it++;
                    return v == (rep >> 32);
                });
                assert(it == set.crend());
            }
            {
                auto val = dist(engine);
                auto it = set.lower_bound(val);
                Slice s(reinterpret_cast<char *>(&val), sizeof(val));
                tree.Visit<tree.kForward>(s, [&it](const uint64_t & rep) {
                    uint32_t v = *it++;
                    return v == (rep >> 32);
                });
                assert(it == set.cend());
            }

            std::string out;
            for (uint32_t v:set) {
                Slice s(reinterpret_cast<char *>(&v), sizeof(v));
                tree.Get(s, &out);
                assert(s == out);

                out.clear();
                tree.Del(s);
                tree.Get(s, &out);
                assert(s != out);
            }
            assert(allocator.records_.size() == 1);
            assert(tree.Size() == 0);

            decltype(set) expect;
            {
                for (uint32_t v:set) {
                    Slice s(reinterpret_cast<char *>(&v), sizeof(v));
                    tree.Add(s, s);
                }

                auto it = set.cbegin();
                tree.VisitDel<tree.kForward>("", [&](const uint64_t & rep) -> std::pair<bool, bool> {
                    uint32_t v = *it++;
                    bool proceed = (v == (rep >> 32));

                    if (std::bernoulli_distribution()(engine)) {
                        return {proceed, true};
                    } else {
                        expect.emplace(v);
                        return {proceed, false};
                    }
                });
                assert(it == set.cend());

                it = expect.cbegin();
                tree.Visit<tree.kForward>("", [&it](const uint64_t & rep) {
                    uint32_t v = *it++;
                    return v == (rep >> 32);
                });
                assert(it == expect.cend());
            }
            {
                Helper dst_helper;
                AllocatorImpl dst_allocator;
                SignatureTreeTpl<KVTrans> dst(&dst_helper, &dst_allocator);
                tree.Rebuild(&dst);

                auto it = expect.cbegin();
                dst.Visit<tree.kForward>("", [&it](const uint64_t & rep) {
                    uint32_t v = *it++;
                    return v == (rep >> 32);
                });
                assert(it == expect.cend());
            }
        };
    }
}