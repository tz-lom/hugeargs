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
#include <unordered_set>
#include <unordered_map>
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
        for(int i = 0; environ[i] != NULL; ++i) \
        { \
            std::cerr << "environ[" << i << "]: " << environ[i] << std::endl; \
        } \
    })

#define DEBUG_NO_REDIRECT(name) DEBUG_PRN({ \
        std::cerr << "Not redirecting " #name " call" << std::endl; \
        for (int i = 0; argv[i] != NULL; ++i) \
        { \
            std::cerr << "argv[" << i << "]: " << argv[i] << std::endl; \
        } \
    })

extern "C" {
int __libc_start_main(int (*main) (int, char * *, char * *), int argc, char * * ubp_av, void (*init) (void), void (*fini) (void), void (*rtld_fini) (void), void (* stack_end));
extern char **environ;

static decltype(&__libc_start_main) real_libc_start_main = NULL;
using execve_fn = int (*)(const char *, char *const[], char *const[]);
using execveat_fn = int (*)(int, const char *, char *const[], char *const[], int);
using fexecve_fn = int (*)(int, char *const[], char *const[]);
using execv_fn = int (*)(const char *, char *const[]);
using execvp_fn = int (*)(const char *, char *const[]);
using execvpe_fn = int (*)(const char *, char *const[], char *const[]);
using posix_spawn_fn = int (*)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
using posix_spawnp_fn = int (*)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
static execve_fn real_execve = NULL;
static execveat_fn real_execveat = NULL;
static fexecve_fn real_fexecve = NULL;
static execve_fn real___execve = NULL;
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

static const std::string ARG_FILE_PREFIX = "--HUGEARGS_PLEASE_LOAD_ARGUMENTS_FROM_FILE=";
static const std::string ARG_FILE_MAGIC = "HUGEARGS_V1";
static constexpr size_t REDIRECT_THRESHOLD_BYTES = 1900000;

struct packed_exec_data
{
    bool has_prefix = false;
    std::vector<std::string> args;
    std::vector<std::string> env;
};

static bool read_nul_section(const std::string &data, std::size_t &pos, std::vector<std::string> &out)
{
    while (pos < data.size())
    {
        const std::size_t end = data.find('\0', pos);
        if (end == std::string::npos)
        {
            return false;
        }

        if (end == pos)
        {
            pos = end + 1;
            return true;
        }

        out.emplace_back(data.substr(pos, end - pos));
        pos = end + 1;
    }

    return false;
}

static packed_exec_data load_argument_file(const std::string &path)
{
    packed_exec_data loaded;
    DEBUG_PRN(std::cerr << "Loading arguments from file: " << path << std::endl;)
    DEBUG_PRN(std::cerr << "Argument file prefix: " << path.substr(0, ARG_FILE_PREFIX.size()) << std::endl;)
    if(path.substr(0, ARG_FILE_PREFIX.size()) != ARG_FILE_PREFIX)
    {
        return loaded;
    }
    loaded.has_prefix = true;
    DEBUG_PRN(std::cerr << "Argument file prefix matched: " << ARG_FILE_PREFIX << std::endl;)
    const std::string file_path = path.substr(ARG_FILE_PREFIX.size());
    std::ifstream input(file_path, std::ios::binary);
    if (!input)
    {
        return loaded;
    }

    std::string file_data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    const std::string magic_with_nul = ARG_FILE_MAGIC + "\0";
    if (file_data.size() >= magic_with_nul.size() && file_data.compare(0, magic_with_nul.size(), magic_with_nul) == 0)
    {
        std::size_t pos = magic_with_nul.size();
        if (!read_nul_section(file_data, pos, loaded.args))
        {
            loaded.args.clear();
            loaded.env.clear();
            return loaded;
        }
        if (!read_nul_section(file_data, pos, loaded.env))
        {
            loaded.args.clear();
            loaded.env.clear();
            return loaded;
        }
        return loaded;
    }

    std::size_t start = 0;
    while (start < file_data.size())
    {
        const std::size_t end = file_data.find('\0', start);
        if (end == std::string::npos)
        {
            if (start < file_data.size())
            {
                loaded.args.emplace_back(file_data.substr(start));
            }
            break;
        }

        if (end > start)
        {
            loaded.args.emplace_back(file_data.substr(start, end - start));
        }
        start = end + 1;
    }

    return loaded;
}

static bool write_hugeargs_file(char *const argv[], char *const envp[], char *out_path, size_t out_size)
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

    const std::string magic_with_nul = ARG_FILE_MAGIC + "\0";
    if (write(fd, magic_with_nul.data(), magic_with_nul.size()) < 0)
    {
        close(fd);
        unlink(template_path);
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

    if (write(fd, "\0", 1) < 0)
    {
        close(fd);
        unlink(template_path);
        return false;
    }

    for (int i = 0; envp != NULL && envp[i] != NULL; ++i)
    {
        const char *env = envp[i];
        const size_t len = strlen(env);
        if (len > 0 && write(fd, env, len) < 0)
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

    if (write(fd, "\0", 1) < 0)
    {
        close(fd);
        unlink(template_path);
        return false;
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

static size_t count_exec_memory_bytes(char *const argv[], char *const envp[])
{
    size_t bytes = 0;
    size_t argc = 0;
    size_t envc = 0;

    for (int i = 0; argv != NULL && argv[i] != NULL; ++i)
    {
        bytes += strlen(argv[i]) + 1;
        ++argc;
    }

    for (int i = 0; envp != NULL && envp[i] != NULL; ++i)
    {
        bytes += strlen(envp[i]) + 1;
        ++envc;
    }

    bytes += (argc + 1 + envc + 1) * sizeof(char *);
    return bytes;
}

static bool should_redirect_exec(char *const argv[], char *const envp[])
{
    if (argv == NULL || argv[0] == NULL)
    {
        return false;
    }

    if (count_exec_memory_bytes(argv, envp) <= REDIRECT_THRESHOLD_BYTES)
    {
        return false;
    }

    if (!ld_preload_contains_self())
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

static size_t count_exec_memory_bytes_with_env_vector(char *const argv[], const std::vector<std::string> &env)
{
    size_t bytes = 0;
    size_t argc = 0;

    for (int i = 0; argv != NULL && argv[i] != NULL; ++i)
    {
        bytes += strlen(argv[i]) + 1;
        ++argc;
    }

    for (const auto &entry : env)
    {
        bytes += entry.size() + 1;
    }

    bytes += (argc + 1 + env.size() + 1) * sizeof(char *);
    return bytes;
}

static std::string env_key(const std::string &entry)
{
    const std::size_t eq = entry.find('=');
    return eq == std::string::npos ? entry : entry.substr(0, eq);
}

static std::vector<std::string> build_runtime_env(char *const source_envp[], char *const redirected_argv[])
{
    std::vector<std::string> env;
    for (int i = 0; source_envp != NULL && source_envp[i] != NULL; ++i)
    {
        env.emplace_back(source_envp[i]);
    }

    if (count_exec_memory_bytes_with_env_vector(redirected_argv, env) <= REDIRECT_THRESHOLD_BYTES)
    {
        return env;
    }

    struct env_item
    {
        std::string entry;
        std::string key;
        bool keep;
        bool dropped;
    };

    const std::unordered_set<std::string> keep_keys = {
        "LD_PRELOAD",
        "PATH",
        "LD_LIBRARY_PATH",
        "HUGEARGS_TMPDIR",
        "HUGEARGS_IGNORE"
    };

    std::vector<env_item> items;
    items.reserve(env.size());
    for (const auto &entry : env)
    {
        const std::string key = env_key(entry);
        items.push_back(env_item{entry, key, keep_keys.find(key) != keep_keys.end(), false});
    }

    auto bytes_with_items = [&]() {
        std::vector<std::string> current;
        current.reserve(items.size());
        for (const auto &item : items)
        {
            if (!item.dropped)
            {
                current.push_back(item.entry);
            }
        }
        return count_exec_memory_bytes_with_env_vector(redirected_argv, current);
    };

    while (bytes_with_items() > REDIRECT_THRESHOLD_BYTES)
    {
        int drop_idx = -1;
        size_t drop_len = 0;

        for (std::size_t i = 0; i < items.size(); ++i)
        {
            if (items[i].dropped || items[i].keep)
            {
                continue;
            }
            if (drop_idx < 0 || items[i].entry.size() > drop_len)
            {
                drop_idx = static_cast<int>(i);
                drop_len = items[i].entry.size();
            }
        }

        if (drop_idx < 0)
        {
            break;
        }

        items[drop_idx].dropped = true;
    }

    std::vector<std::string> filtered;
    filtered.reserve(items.size());
    for (const auto &item : items)
    {
        if (!item.dropped)
        {
            filtered.push_back(item.entry);
        }
    }

    return filtered;
}

static std::vector<char *> env_ptrs_from_strings(std::vector<std::string> &env)
{
    std::vector<char *> ptrs;
    ptrs.reserve(env.size() + 1);
    for (auto &entry : env)
    {
        ptrs.push_back(const_cast<char *>(entry.c_str()));
    }
    ptrs.push_back(nullptr);
    return ptrs;
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
    char *const *effective_envp = envp != NULL ? envp : environ;
    if (should_redirect_exec(argv, const_cast<char *const *>(effective_envp)))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, const_cast<char *const *>(effective_envp), tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(const_cast<char *const *>(effective_envp), new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            DEBUG_REDIRECT(execve, pathname);
            const int rc = real_execve(pathname, new_argv, runtime_envp.data());
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(execve);
    }

    return real_execve(pathname, argv, envp);
}

int __execve(const char *pathname, char *const argv[], char *const envp[])
{
    char *const *effective_envp = envp != NULL ? envp : environ;
    if (should_redirect_exec(argv, const_cast<char *const *>(effective_envp)))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, const_cast<char *const *>(effective_envp), tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(const_cast<char *const *>(effective_envp), new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            DEBUG_REDIRECT(__execve, pathname);
            const int rc = real___execve(pathname, new_argv, runtime_envp.data());
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(__execve);
    }

    return real___execve(pathname, argv, envp);
}

int execveat(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags)
{
    char *const *effective_envp = envp != NULL ? envp : environ;
    if (should_redirect_exec(argv, const_cast<char *const *>(effective_envp)))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, const_cast<char *const *>(effective_envp), tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(const_cast<char *const *>(effective_envp), new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            DEBUG_REDIRECT(execveat, pathname);
            const int rc = real_execveat(dirfd, pathname, new_argv, runtime_envp.data(), flags);
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(execveat);
    }

    return real_execveat(dirfd, pathname, argv, envp, flags);
}

int fexecve(int fd, char *const argv[], char *const envp[])
{
    char *const *effective_envp = envp != NULL ? envp : environ;
    if (should_redirect_exec(argv, const_cast<char *const *>(effective_envp)))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, const_cast<char *const *>(effective_envp), tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(const_cast<char *const *>(effective_envp), new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            DEBUG_REDIRECT(fexecve, "<fd>");
            const int rc = real_fexecve(fd, new_argv, runtime_envp.data());
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(fexecve);
    }

    return real_fexecve(fd, argv, envp);
}

int execv(const char *path, char *const argv[] )
{
    if (should_redirect_exec(argv, environ))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, environ, tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(environ, new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            char **saved_environ = environ;
            environ = runtime_envp.data();
            DEBUG_REDIRECT(execv, path);
            const int rc = real_execv(path, new_argv);
            environ = saved_environ;
            DEBUG_PRN(std::cerr << "execv result: " << rc << std::endl;)
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(execv);
    }

    return real_execv(path, argv);
}

int execvp(const char *file, char *const argv[])
{
    if (should_redirect_exec(argv, environ))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, environ, tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(environ, new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            char **saved_environ = environ;
            environ = runtime_envp.data();
            DEBUG_REDIRECT(execvp, file);
            const int rc = real_execvp(file, new_argv);
            environ = saved_environ;
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(execvp);
    }

    return real_execvp(file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    char *const *effective_envp = envp != NULL ? envp : environ;
    if (should_redirect_exec(argv, const_cast<char *const *>(effective_envp)))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, const_cast<char *const *>(effective_envp), tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(const_cast<char *const *>(effective_envp), new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            DEBUG_REDIRECT(execvpe, file);
            const int rc = real_execvpe(file, new_argv, runtime_envp.data());
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(execvpe);
    }

    return real_execvpe(file, argv, envp);
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions, const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    char *const *effective_envp = envp != NULL ? envp : environ;
    if (should_redirect_exec(argv, const_cast<char *const *>(effective_envp)))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, const_cast<char *const *>(effective_envp), tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(const_cast<char *const *>(effective_envp), new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            DEBUG_REDIRECT(posix_spawn, path);
            const int rc = real_posix_spawn(pid, path, file_actions, attrp, new_argv, runtime_envp.data());
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(posix_spawn);
    }

    return real_posix_spawn(pid, path, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions, const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    char *const *effective_envp = envp != NULL ? envp : environ;
    if (should_redirect_exec(argv, const_cast<char *const *>(effective_envp)))
    {
        char tmp_path[PATH_MAX];
        if (write_hugeargs_file(argv, const_cast<char *const *>(effective_envp), tmp_path, sizeof(tmp_path)))
        {
            std::string file_arg = ARG_FILE_PREFIX+tmp_path;
            char *new_argv[] = { argv[0], const_cast<char *>(file_arg.c_str()), NULL };
            std::vector<std::string> runtime_env = build_runtime_env(const_cast<char *const *>(effective_envp), new_argv);
            std::vector<char *> runtime_envp = env_ptrs_from_strings(runtime_env);
            DEBUG_REDIRECT(posix_spawnp, file);
            const int rc = real_posix_spawnp(pid, file, file_actions, attrp, new_argv, runtime_envp.data());
            return rc;
        }
    }
    else
    {
        DEBUG_NO_REDIRECT(posix_spawnp);
    }

    return real_posix_spawnp(pid, file, file_actions, attrp, argv, envp);
}

int __libc_start_main(int (*main) (int, char * *, char * *), int argc, char * * ubp_av, void (*init) (void), void (*fini) (void), void (*rtld_fini) (void), void (* stack_end))
{
    if (argc == 2)
    {
        DEBUG_PRN(std::cerr << "Checking for argument file: " << ubp_av[1] << std::endl;)
        const packed_exec_data loaded_data = load_argument_file(ubp_av[1]);
        if (loaded_data.has_prefix)
        {
            std::deque<std::string> owned_strings;
            std::vector<char *> argv_storage;
            std::vector<std::string> merged_env;
            std::unordered_map<std::string, std::size_t> env_index;

            for (char **env = ubp_av + argc + 1; env != nullptr && *env != nullptr; ++env)
            {
                merged_env.emplace_back(*env);
            }

            for (std::size_t i = 0; i < merged_env.size(); ++i)
            {
                const std::string &entry = merged_env[i];
                const std::size_t eq = entry.find('=');
                const std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
                env_index[key] = i;
            }

            for (const auto &entry : loaded_data.env)
            {
                const std::size_t eq = entry.find('=');
                const std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
                auto it = env_index.find(key);
                if (it == env_index.end())
                {
                    env_index[key] = merged_env.size();
                    merged_env.push_back(entry);
                }
                else
                {
                    merged_env[it->second] = entry;
                }
            }

            argv_storage.reserve(loaded_data.args.size() + merged_env.size() + 4);

            argv_storage.push_back(ubp_av[0]);
            for (const auto &arg : loaded_data.args)
            {
                owned_strings.emplace_back(arg);
                argv_storage.push_back(const_cast<char *>(owned_strings.back().c_str()));
            }

            argv_storage.push_back(nullptr);
            for (const auto &env : merged_env)
            {
                owned_strings.emplace_back(env);
                argv_storage.push_back(const_cast<char *>(owned_strings.back().c_str()));
            }
            argv_storage.push_back(nullptr);

            const int new_argc = static_cast<int>(1 + loaded_data.args.size());

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
    real_execveat = (execveat_fn)dlsym(RTLD_NEXT, "execveat");
    real_fexecve = (fexecve_fn)dlsym(RTLD_NEXT, "fexecve");
    real___execve = (execve_fn)dlsym(RTLD_NEXT, "__execve");
    real_execv = (execv_fn)dlsym(RTLD_NEXT, "execv");
    real_execvp = (execvp_fn)dlsym(RTLD_NEXT, "execvp");
    real_execvpe = (execvpe_fn)dlsym(RTLD_NEXT, "execvpe");
    real_posix_spawn = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    real_posix_spawnp = (posix_spawnp_fn)dlsym(RTLD_NEXT, "posix_spawnp");
    init_self_name();
}

}
