#include <iostream>

namespace sgt {
    namespace sig_tree_test {
        void Run();
    }
    namespace sig_tree_bench {
        void Run();
    }
}

using namespace sgt;

int main() {
    sig_tree_test::Run();
    sig_tree_bench::Run();
    std::cout << "Done." << std::endl;
    return 0;
}