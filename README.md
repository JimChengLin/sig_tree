# sig_tree
Read ./bench/sig_tree_bench.cpp

1M 16B Keys Benchmark
```
SGT - Add took 941 milliseconds
std::set - emplace took 1027 milliseconds
std::unordered_set - emplace took 440 milliseconds
SGT - Get took 664 milliseconds
std::set - find took 1381 milliseconds
std::unordered_set - find took 192 milliseconds
```