#include "app.hpp"

int main(int argc, char** argv) {
    App app;
    if (argc > 0 && argv && argv[0]) app.selfPath = argv[0];
    if (app.init()) app.run();
    app.shutdown();
    return 0;
}
