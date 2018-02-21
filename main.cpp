#include <iostream>

namespace sgt {
    namespace sig_tree_test {
        void Run();
    }
    namespace sig_tree_bench {
        void Run();
    }
}

int main() {
    sgt::sig_tree_test::Run();
    sgt::sig_tree_bench::Run();
    std::cout << "Done." << std::endl;
    return 0;
}