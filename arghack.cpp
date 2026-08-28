#include <deque>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
extern char **environ;

int __libc_start_main(int (*main) (int, char * *, char * *), int argc, char * * ubp_av, void (*init) (void), void (*fini) (void), void (*rtld_fini) (void), void (* stack_end));

static decltype(&__libc_start_main) real_libc_start_main = NULL;

static std::vector<std::string> load_argument_file(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    std::string file_data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::vector<std::string> args;

    std::size_t start = 0;
    while (start < file_data.size())
    {
        const std::size_t end = file_data.find('\0', start);
        if (end == std::string::npos)
        {
            if (start < file_data.size())
            {
                args.emplace_back(file_data.substr(start));
            }
            break;
        }

        if (end > start)
        {
            args.emplace_back(file_data.substr(start, end - start));
        }
        start = end + 1;
    }

    return args;
}

int __libc_start_main(int (*main) (int, char * *, char * *), int argc, char * * ubp_av, void (*init) (void), void (*fini) (void), void (*rtld_fini) (void), void (* stack_end))
{
    if (argc == 2)
    {
        const std::vector<std::string> loaded_args = load_argument_file(ubp_av[1]);
        if (!loaded_args.empty())
        {
            std::deque<std::string> owned_strings;
            std::vector<char *> argv_storage;
            argv_storage.reserve(loaded_args.size() + 2 + 16);

            argv_storage.push_back(ubp_av[0]);
            for (const auto &arg : loaded_args)
            {
                owned_strings.emplace_back(arg);
                argv_storage.push_back(const_cast<char *>(owned_strings.back().c_str()));
            }

            argv_storage.push_back(nullptr);
            for (char **env = environ; env != nullptr && *env != nullptr; ++env)
            {
                owned_strings.emplace_back(*env);
                argv_storage.push_back(const_cast<char *>(owned_strings.back().c_str()));
            }
            argv_storage.push_back(nullptr);

            const int new_argc = static_cast<int>(1 + loaded_args.size());
            return real_libc_start_main(main, new_argc, argv_storage.data(), init, fini, rtld_fini, stack_end);
        }
    }

    return real_libc_start_main(main, argc, ubp_av, init, fini, rtld_fini, stack_end);
}

__attribute__((constructor)) static void on_load()
{
    dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
    real_libc_start_main = (decltype(&__libc_start_main))dlsym(RTLD_NEXT, "__libc_start_main");
}

}