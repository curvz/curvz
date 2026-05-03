#include "Application.hpp"

int main(int argc, char* argv[]) {
    auto app = Curvz::Application::create();
    return app->run(argc, argv);
}
