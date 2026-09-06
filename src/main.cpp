#include "faultline/config.hpp"
#include "faultline/control_server.hpp"
#include "faultline/proxy.hpp"

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace
{

volatile std::sig_atomic_t interrupted = 0;

void handle_signal(int)
{
    interrupted = 1;
}

void print_usage(std::ostream &out)
{
    out << "Usage:\n"
        << "  faultline run --config <path>\n"
        << "  faultline check --config <path>\n"
        << "  faultline --help\n"
        << "  faultline --version\n";
}

}

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2)
        {
            const std::string option = argv[1];
            if (option == "--help" || option == "-h")
            {
                print_usage(std::cout);
                return 0;
            }
            if (option == "--version")
            {
                std::cout << "faultline " << FAULTLINE_VERSION << '\n';
                return 0;
            }
        }
        if (argc != 4 || std::string(argv[2]) != "--config")
        {
            print_usage(std::cerr);
            return 2;
        }
        const std::string command = argv[1];
        const auto scenario = faultline::load_scenario(argv[3]);
        if (command == "check")
        {
            std::cout << faultline::describe(scenario) << '\n';
            return 0;
        }
        if (command != "run")
        {
            print_usage(std::cerr);
            return 2;
        }

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        faultline::ProxyServer server(scenario, std::cout);
        faultline::ControlServer control(server, std::cout);
        std::jthread control_thread([&control, &server](const std::stop_token &token) {
            try
            {
                control.run(token);
            }
            catch (const std::exception &error)
            {
                std::cerr << "faultline control: " << error.what() << '\n';
                server.request_stop();
            }
        });
        std::jthread signal_watcher([&server](const std::stop_token &token) {
            while (!token.stop_requested() && interrupted == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (interrupted != 0)
                server.request_stop();
        });
        server.run();
        control.request_stop();
        control_thread.request_stop();
        signal_watcher.request_stop();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "faultline: " << error.what() << '\n';
        return 1;
    }
}
