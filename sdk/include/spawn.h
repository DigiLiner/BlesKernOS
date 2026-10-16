#ifndef BLESKERNOS_POSIX_SPAWN_H
#define BLESKERNOS_POSIX_SPAWN_H

#include <sys/types.h>

typedef struct { unsigned int flags; } posix_spawnattr_t;
#define POSIX_SPAWN_ACTION_MAX 8
typedef struct {
    unsigned int action_count;
    struct { int type,fd,new_fd,flags; mode_t mode; char path[260]; }
        actions[POSIX_SPAWN_ACTION_MAX];
} posix_spawn_file_actions_t;

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attributes,
                char *const argv[], char *const envp[]);
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes,
                 char *const argv[], char *const envp[]);
int posix_spawnattr_init(posix_spawnattr_t *attributes);
int posix_spawnattr_destroy(posix_spawnattr_t *attributes);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions,
                                      int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions,
                                     int old_fd, int new_fd);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
                                     int fd, const char *path, int flags,
                                     mode_t mode);

#endif
