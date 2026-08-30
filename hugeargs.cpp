#include <deque>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <libgen.h>
#include <sstream>
#include <spawn.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <vector>

#ifdef DEBUG
#define DEBUG_PRN(x) x
#else
#define DEBUG_PRN(x)
#endif

#define DEBUG_REDIRECT(name, path) DEBUG_PRN({ \
        std::cerr << "Redirecting "#name" call for: " << path << std::endl; \
        for (int i = 0; argv[i] != NULL; ++i) \
        { \
            std::cerr << "argv[" << i << "]: " << argv[i] << std::endl; \
        } \
        std::cerr << "to" << std::endl; \
        for (int i = 0; new_argv[i] != NULL; ++i) \
        { \
            std::cerr << "new_argv[" << i << "]: " << new_argv[i] << std::endl; \
        } \
    })

extern "C" {
int __libc_start_main(int (*main) (int, char * *, char * *), int argc, char * * ubp_av, void (*init) (void), void (*fini) (void), void (*rtld_fini) (void), void (* stack_end));

static decltype(&__libc_start_main) real_libc_start_main = NULL;
using execve_fn = int (*)(const char *, char *const[], char *const[]);
using execv_fn = int (*)(const char *, char *const[]);
using execvp_fn = int (*)(const char *, char *const[]);
using execvpe_fn = int (*)(const char *, char *const[], char *const[]);
using posix_spawn_fn = int (*)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
using posix_spawnp_fn = int (*)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
static execve_fn real_execve = NULL;
static execv_fn real_execv = NULL;
static execvp_fn real_execvp = NULL;
static execvpe_fn real_execvpe = NULL;
static posix_spawn_fn real_posix_spawn = NULL;
static posix_spawnp_fn real_posix_spawnp = NULL;

static char g_self_name[PATH_MAX] = {0};

static void init_self_name()
{
    if (g_self_name[0] != '\0')
    {
        return;
    }

    Dl_info info;
    if (dladdr((void *)&__libc_start_main, &info) != 0 && info.dli_fname != NULL)
    {
        const char *path = info.dli_fname;
        const char *base = strrchr(path, '/');
        snprintf(g_self_name, sizeof(g_self_name), "%s", base ? base + 1 : path);
    }
    else
    {
        snprintf(g_self_name, sizeof(g_self_name), "libhugeargs.so");
    }
}

static bool ld_preload_contains_self()
{
    init_self_name();
    const char *preload = getenv("LD_PRELOAD");
    if (preload == NULL)
    {
        return false;
    }

    return strstr(preload, g_self_name) != NULL;
}

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

static bool write_hugeargs_file(char *const argv[], char *out_path, size_t out_size)
{
    const char *tmpdir = getenv("HUGEARGS_TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0')
    {
        return false;
    }

    char template_path[PATH_MAX];
    snprintf(template_path, sizeof(template_path), "%s/hugeargs-XXXXXX", tmpdir);

    const int fd = mkstemp(template_path);
    if (fd < 0)
    {
        return false;
    }

    for (int i = 0; argv != NULL && argv[i] != NULL; ++i)
    {
        if (i == 0)
        {
            continue;
        }

        const char *arg = argv[i];
        const size_t len = strlen(arg);
        if (len > 0 && write(fd, arg, len) < 0)
        {
            close(fd);
            unlink(template_path);
            return false;
        }
        if (write(fd, "\0", 1) < 0)
        {
            close(fd);
            unlink(template_path);
            return false;
        }
    }

    close(fd);
    snprintf(out_path, out_size, "%s", template_path);
    return true;
}

static bool should_ignore_process(const char *path)
{
    const char *ignore = getenv("HUGEARGS_IGNORE");
    if (ignore == NULL || ignore[0] == '\0')
    {
        return false;
    }

    const std::string ignore_list(ignore);
    std::string name = path;
    const std::size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
    {
        name = name.substr(slash + 1);
    }

    DEBUG_PRN(std::cerr << "Checking if process should be ignored: " << name << std::endl;)

    std::size_t start = 0;
    while (start < ignore_list.size())
    {
        const std::size_t end = ignore_list.find(':', start);
        const std::string item = ignore_list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty() && item == name)
        {
            DEBUG_PRN(std::cerr << "Process " << name << " is in the ignore list." << std::endl;)
            return true;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }

    return false;
}

static bool should_redirect_exec(char *const argv[])
{
    if (!ld_preload_contains_self())
    {
        return false;
    }

    if (argv == NULL || argv[0] == NULL)
    {
        return false;
    }

    if (should_ignore_process(argv[0]))
    {
        return false;
    }

    if (getenv("HUGEARGS_TMPDIR") == NULL || getenv("HUGEARGS_TMPDIR")[0] == '\0')
    {
        return false;
    }

    return true;
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
    if (should_redirect_exec(argv))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, tmp_path, sizeof(tmp_path)))
        {
            char *file_arg = strdup(tmp_path);
            char *new_argv[] = { argv[0], file_arg, NULL };
            DEBUG_REDIRECT(execve, pathname);
            const int rc = real_execve(pathname, new_argv, envp);
            free(file_arg);
            return rc;
        }
    }

    return real_execve(pathname, argv, envp);
}

int execv(const char *path, char *const argv[] )
{
    if (should_redirect_exec(argv))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, tmp_path, sizeof(tmp_path)))
        {
            char *file_arg = strdup(tmp_path);
            char *new_argv[] = { argv[0], file_arg, NULL };
            DEBUG_REDIRECT(execv, path);
            const int rc = real_execv(path, new_argv);
            free(file_arg);
            return rc;
        }
    }

    return real_execv(path, argv);
}

int execvp(const char *file, char *const argv[])
{
    if (should_redirect_exec(argv))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, tmp_path, sizeof(tmp_path)))
        {
            char *file_arg = strdup(tmp_path);
            char *new_argv[] = { argv[0], file_arg, NULL };
            DEBUG_REDIRECT(execvp, file);
            const int rc = real_execvp(file, new_argv);
            free(file_arg);
            return rc;
        }
    }

    return real_execvp(file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    if (should_redirect_exec(argv))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, tmp_path, sizeof(tmp_path)))
        {
            char *file_arg = strdup(tmp_path);
            char *new_argv[] = { argv[0], file_arg, NULL };
            DEBUG_REDIRECT(execvpe, file);
            const int rc = real_execve(file, new_argv, envp);
            free(file_arg);
            return rc;
        }
    }

    return real_execvpe(file, argv, envp);
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions, const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    if (should_redirect_exec(argv))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, tmp_path, sizeof(tmp_path)))
        {
            char *file_arg = strdup(tmp_path);
            char *new_argv[] = { argv[0], file_arg, NULL };
            DEBUG_REDIRECT(posix_spawn, path);
            const int rc = real_posix_spawn(pid, path, file_actions, attrp, new_argv, envp);
            free(file_arg);
            return rc;
        }
    }

    return real_posix_spawn(pid, path, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions, const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    if (should_redirect_exec(argv))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, tmp_path, sizeof(tmp_path)))
        {
            char *file_arg = strdup(tmp_path);
            char *new_argv[] = { argv[0], file_arg, NULL };
            DEBUG_REDIRECT(posix_spawnp, file);
            const int rc = real_posix_spawnp(pid, file, file_actions, attrp, new_argv, envp);
            free(file_arg);
            return rc;
        }
    }

    return real_posix_spawnp(pid, file, file_actions, attrp, argv, envp);
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
            for (char **env = ubp_av + argc + 1; env != nullptr && *env != nullptr; ++env)
            {
                owned_strings.emplace_back(*env);
                argv_storage.push_back(const_cast<char *>(owned_strings.back().c_str()));
            }
            argv_storage.push_back(nullptr);

            const int new_argc = static_cast<int>(1 + loaded_args.size());

            DEBUG_PRN({
                std::cerr << "Loaded arguments from file: " << ubp_av[1] << std::endl;
                std::cerr << "New argc: " << new_argc << std::endl;
                for (int i = 0; i < new_argc; ++i)
                {
                    std::cerr << "argv[" << i << "]: " << argv_storage[i] << std::endl;
                }
            })

            return real_libc_start_main(main, new_argc, argv_storage.data(), init, fini, rtld_fini, stack_end);
        }
    }
    else
    {
        DEBUG_PRN({
            std::cerr << "Not loading arguments from file. argc: " << argc << std::endl;
            for (int i = 0; i < argc; ++i)
            {
                std::cerr << "argv[" << i << "]: " << ubp_av[i] << std::endl;
            }
        });
    }

    return real_libc_start_main(main, argc, ubp_av, init, fini, rtld_fini, stack_end);
}

__attribute__((constructor)) static void on_load()
{
    real_libc_start_main = (decltype(&__libc_start_main))dlsym(RTLD_NEXT, "__libc_start_main");
    real_execve = (execve_fn)dlsym(RTLD_NEXT, "execve");
    real_execv = (execv_fn)dlsym(RTLD_NEXT, "execv");
    real_execvp = (execvp_fn)dlsym(RTLD_NEXT, "execvp");
    real_execvpe = (execvpe_fn)dlsym(RTLD_NEXT, "execvpe");
    real_posix_spawn = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    real_posix_spawnp = (posix_spawnp_fn)dlsym(RTLD_NEXT, "posix_spawnp");
    init_self_name();
}

}
