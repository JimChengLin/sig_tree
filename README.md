# sig_tree
用法参见 bench 目录下的 sig_tree_bench.cpp
理论上是 header-only 库(拷贝src目录下所有的头文件然后引入 sig_tree.h及其impl.h即可), 但因为我用了很多 constexpr, 需要 C++ 17 的支持

来源于我人生中最失败的项目levidb
我发现写完一个速度足够**快**的数据库存储引擎太花时间了. 我也得不到什么物质利益.

所以我把最核心的数据结构 BDT 抽离了出来做成了模板+接口的形式.
原项目是AGPL的, 这里改成MIT, 为了区分 BDT 也就有了新名字 SigTree-签名树.

我有了更重要的事情和新的兴趣... 为了不让我冥思苦想的产物被埋没, 特此发表.

大体上的结果是这样的.
SGT - Add took 2787 milliseconds
std::set - emplace took 1172 milliseconds
SGT - Get took 1353 milliseconds
std::set - find took 1553 milliseconds
sig_tree_cmp_times: 1999999
std_set_cmp_times: 49859683

写入100W条16Bytes的随机字符串, sigtree的速度仅为std::set的二到三分之一.
读取速度55开.

但sigtree原来就是被设计用于硬盘索引, 所以更重要的是指标是key的比较次数.
std::set的实现我估计是红黑树, 进行了49741250次key的比较. 作为硬盘索引这就是49741250次硬盘读取操作.
而sigtree仅有1999999次, 是其20分之一!

我很长一段时间内应该都不会再冒泡了. 大家新年快乐~
