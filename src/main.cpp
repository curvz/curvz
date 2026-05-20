#include "Application.hpp"

int main(int argc, char* argv[]) {
    // s268 m0 — stamp the cold-launch start as the very first
    // statement in main(). Everything after this — Application::create,
    // GTK init, window construction, first paint — counts toward the
    // "exec -> first paint" number that Canvas::on_draw will log when
    // the first frame lands.
    Curvz::g_launch_t0 = std::chrono::steady_clock::now();

    auto app = Curvz::Application::create();
    return app->run(argc, argv);
}
