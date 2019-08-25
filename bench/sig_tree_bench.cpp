#include <chrono>
#include <iostream>
#include <random>
#include <set>
#include <unordered_set>

#include "../src/sig_tree.h"
#include "../src/sig_tree_impl.h"
#include "../src/sig_tree_node_impl.h"
#include "../src/sig_tree_rebuild_impl.h"
#include "../src/sig_tree_visit_impl.h"

namespace sgt::sig_tree_bench {
    // 字符串比较次数
    static unsigned int sig_tree_cmp_times = 0;
    static unsigned int std_set_cmp_times = 0;

    /*
     * 将 SGT 中表示 KV 的 Token
     * 翻译还原为 Key 和 Value 的代理类
     * 大体为 Token => KV 的单向映射
     *
     * 这里的实现没有区分 Key 和 Value
     */
    class KVTrans {
    private:
        // KV 在内存中的表达是 C 式字符串
        const char * s_;

    public:
        explicit KVTrans(const char * s) : s_(s) {}

    public:
        bool operator==(const Slice & k) const {
            ++sig_tree_cmp_times;
            return strcmp(k.data(), s_) == 0;
        }

        Slice Key() const {
            return {s_};
        }

        bool Get(const Slice & k, std::string * v) const {
            if (*this == k) {
                if (v != nullptr) {
                    v->assign(s_);
                }
                return true;
            }
            return false;
        }

    public:
        static uint64_t Pack(size_t offset) {
            return offset + 1;
        }

        static size_t Unpack(const uint64_t & rep) {
            return rep - 1;
        }

        static bool IsPacked(const uint64_t & rep) {
            return rep % 2 == 1;
        }
    };

    /*
     * Helper 接口定义了如何生成和使用 KV Token
     */
    class Helper final : public SignatureTreeTpl<KVTrans>::Helper {
    public:
        ~Helper() override = default;

    public:
        // 根据要存储的 KV 返回一个 Token
        uint64_t Add(const Slice & k, const Slice & v) override {
            // 我确信 k.data() 会返回一个外部的 C 式字符串
            // 如果需要交接资源所有权, 可以在这里进行移动/复制
            return reinterpret_cast<uintptr_t>(k.data());
        }

        // 释放资源, 由于 KV 的所有权在外部, 这里不需要任何操作
        void Del(KVTrans & trans) override {}

        // Allocator.AllocatePage() 后获得的 offset 必须要能够打包进 Token
        // 言下之意就是 Token(默认类型 uint64_t) 的空间内必须能自省表达两种数据
        // union Token {
        //   TokenByAdd a;
        //   TokenByAllocatePage b;
        // }
        //
        // Add 和 AllocatePage 必然返回偶数
        // 后者 +1 成为奇数, 通过奇偶性区分二者
        uint64_t Pack(size_t offset) const override {
            return offset + 1;
        }

        size_t Unpack(const uint64_t & rep) const override {
            return rep - 1;
        }

        bool IsPacked(const uint64_t & rep) const override {
            return rep % 2 == 1;
        }

        KVTrans Trans(const uint64_t & rep) const override {
            // Token(rep) => KVTrans => KV
            return KVTrans(reinterpret_cast<char *>(static_cast<uintptr_t>(rep)));
        }
    };

    /*
     * 内存分配器
     *
     * 如果分配在 file-backed mmap 上可作为硬盘索引
     * 直接 malloc 就是内存索引
     */
    class AllocatorImpl final : public Allocator {
    public:
        std::unordered_set<uintptr_t> records_;

    public:
        // 释放已分配的内存
        ~AllocatorImpl() override {
            for (uintptr_t record:records_) {
                free(reinterpret_cast<void *>(record));
            }
        }

    public:
        // Base() + AllocatePage() 返回的 offset = 真实内存位置
        // 合理性在于如果使用 mmap, 有可能需要 re-mmap
        // 相同的偏移量永远能得到相同的内容, 尽管 Base() 可能返回不同的值
        // 如果是内存索引直接返回 0(nullptr) 即可
        void * Base() override {
            return nullptr;
        }

        // 分配一页内存, 大小为 kPageSize
        // 如果是 mmap 且需要扩容才能完成分配, 务必 throw AllocatorFullException
        // SGT 会捕获这一异常并调用 Grow(), 再根据 Base() 重新计算内存位置
        size_t AllocatePage() override {
            auto page = reinterpret_cast<uintptr_t>(malloc(kPageSize));
            records_.emplace(page);
            return page;
        }

        // 释放内存页
        void FreePage(size_t offset) override {
            auto it = records_.find(offset);
            free(reinterpret_cast<void *>(*it));
            records_.erase(it);
        }

        // 扩容
        void Grow() override {}
    };

#define TIME_START auto start = std::chrono::high_resolution_clock::now()
#define TIME_END auto end = std::chrono::high_resolution_clock::now()
#define PRINT_TIME(name) \
std::cout << name " took " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << " milliseconds" << std::endl

    void Run() {
        auto seed = std::random_device()();
        std::default_random_engine engine(seed);
        std::uniform_int_distribution<uint8_t> dist(1);

        // 随机生成 100W 16Bytes C 式字符串
        std::vector<uint8_t *> src(1000000);
        for (auto & s:src) {
            s = static_cast<uint8_t *>(malloc(16));
            for (size_t i = 0; i < 15; ++i) {
                s[i] = dist(engine);
            }
            s[15] = 0;
        }

        // 初始化 SGT
        Helper helper;
        AllocatorImpl allocator;
        SignatureTreeTpl<KVTrans> tree(&helper, &allocator);

        // 初始化 std::set
        struct cmp {
            bool operator()(const char * a, const char * b) const {
                ++std_set_cmp_times;
                return strcmp(a, b) < 0;
            }
        };
        std::set<const char *, cmp> set;

        // 初始化 std::unordered_set
        struct hash {
            size_t operator()(const char * a) const {
                return SliceHasher()(Slice(a));
            }
        };
        struct equal {
            bool operator()(const char * a, const char * b) const {
                return strcmp(a, b) == 0;
            }
        };
        std::unordered_set<const char *, hash, equal> unordered_set;

        // Add - 开始
        {
            TIME_START;
            for (const auto & s:src) {
                tree.Add(reinterpret_cast<char *>(s), {});
            }
            TIME_END;
            PRINT_TIME("SGT - Add");
        }
        {
            TIME_START;
            for (const auto & s:src) {
                set.emplace(reinterpret_cast<char *>(s));
            }
            TIME_END;
            PRINT_TIME("std::set - emplace");
        }
        {
            TIME_START;
            for (const auto & s:src) {
                unordered_set.emplace(reinterpret_cast<char *>(s));
            }
            TIME_END;
            PRINT_TIME("std::unordered_set - emplace");
        }
        // Add - 结束
        {
            TIME_START;
            tree.Compact();
            TIME_END;
            PRINT_TIME("SGT - Compact");
        }
        // Get - 开始
        {
            TIME_START;
            for (const auto & s:src) {
                tree.Get(reinterpret_cast<char *>(s), nullptr);
            }
            TIME_END;
            PRINT_TIME("SGT - Get");
        }
        {
            TIME_START;
            for (const auto & s:src) {
                set.find(reinterpret_cast<char *>(s));
            }
            TIME_END;
            PRINT_TIME("std::set - find");
        }
        {
            TIME_START;
            for (const auto & s:src) {
                unordered_set.find(reinterpret_cast<char *>(s));
            }
            TIME_END;
            PRINT_TIME("std::unordered_set - find");
        }
        // Get - 结束

        // 统计
        std::cout << "sig_tree_cmp_times: " << sig_tree_cmp_times << std::endl;
        std::cout << "sig_tree_mem_pages: " << allocator.records_.size() << std::endl;
        std::cout << "std_set_cmp_times : " << std_set_cmp_times << std::endl;

        {
            Helper helper_rebuild;
            AllocatorImpl allocator_rebuild;
            SignatureTreeTpl<KVTrans> tree_rebuild(&helper_rebuild, &allocator_rebuild);
            {
                TIME_START;
                tree.Rebuild(&tree_rebuild);
                TIME_END;
                PRINT_TIME("SGT - Rebuild");
            }
            {
                TIME_START;
                tree_rebuild.VisitDel<tree.kBackward>({}, [](auto) {
                    return std::make_pair(true, true);
                });
                TIME_END;
                PRINT_TIME("SGT - VisitDel");
                assert(tree_rebuild.Size() == 0);
            }
        }
        {
            TIME_START;
            tree.Visit<tree.kForward>({}, [](auto) { return true; });
            TIME_END;
            PRINT_TIME("SGT - Visit");
        }
        {
            TIME_START;
            for (const auto & s:src) {
                tree.Del(reinterpret_cast<char *>(s));
            }
            TIME_END;
            PRINT_TIME("SGT - Del");
        }

        for (auto & s:src) {
            free(s);
        }
    }
}