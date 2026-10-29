// Runs every registered test and prints how many failed.
#include <sstream>

#include "Testing.h"

int main() {
    using namespace editor::testing;
    for (const Test& test : tests()) {
        int before = failures();
        try {
            test.body();
        } catch (const std::exception& error) {
            fail(test.name.c_str(), 0, std::string("threw ") + error.what());
        }
        if (failures() > before) std::cerr << "FAILED " << test.name << "\n";
    }
    std::cout << tests().size() << " tests, " << failures() << " failures\n";
    return failures() == 0 ? 0 : 1;
}
