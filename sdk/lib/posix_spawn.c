#include "bleskernos.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SPAWN_COMMAND_CAPACITY 2048U

static int append_character(char *out, unsigned int *used, char character) {
    if (*used + 1U >= SPAWN_COMMAND_CAPACITY) return 0;
    out[(*used)++] = character;
    out[*used] = '\0';
    return 1;
}

static int build_command_line(char *out, char *const argv[]) {
    unsigned int used = 0U;
    out[0] = '\0';
    if (!argv) return 1;
    for (unsigned int index = 0U; argv[index]; index++) {
        const char *arg = argv[index];
        int quote = !arg[0] || strchr(arg, ' ') || strchr(arg, '\t') ||
                    strchr(arg, '\"') || strchr(arg, '\'');
        if (index && !append_character(out, &used, ' ')) return 0;
        if (quote && !append_character(out, &used, '\"')) return 0;
        while (*arg) {
            if ((*arg == '\\' || *arg == '\"') &&
                !append_character(out, &used, '\\')) return 0;
            if (!append_character(out, &used, *arg++)) return 0;
        }
        if (quote && !append_character(out, &used, '\"')) return 0;
    }
    return 1;
}

static int spawn_common(pid_t *pid, const char *path,
                        const posix_spawn_file_actions_t *actions,
                        const posix_spawnattr_t *attributes,
                        char *const argv[], char *const envp[]) {
    char command[SPAWN_COMMAND_CAPACITY];
    bk_i32 result;
    int saved[POSIX_SPAWN_ACTION_MAX];
    int targets[POSIX_SPAWN_ACTION_MAX];
    unsigned int applied=0U;
    if (!pid || !path || !path[0]) return EINVAL;
    if (attributes && attributes->flags)
        return ENOSYS;
    /* El kernel aun no hereda un bloque de entorno. El entorno vacio o el
     * entorno POSIX local por defecto son aceptados sin inventar semantica. */
    if (envp && envp[0]) return ENOSYS;
    if (!build_command_line(command, argv)) return E2BIG;
    if(actions)for(unsigned int i=0U;i<actions->action_count;i++){
        const typeof(actions->actions[0])*action=&actions->actions[i];
        int temporary=-1,save_errno;
        targets[applied]=action->type==2?action->new_fd:action->fd;
        saved[applied]=dup(targets[applied]);
        save_errno=errno;
        if(saved[applied]<0&&save_errno!=EBADF)goto action_failed;
        if(action->type==1){if(close(action->fd)<0)goto action_failed;}
        else if(action->type==2){if(dup2(action->fd,action->new_fd)<0)goto action_failed;}
        else if(action->type==3){temporary=open(action->path,action->flags,action->mode);if(temporary<0||dup2(temporary,action->fd)<0){if(temporary>=0)close(temporary);goto action_failed;}close(temporary);}
        else{errno=EINVAL;goto action_failed;}
        applied++;continue;
action_failed:
        if(saved[applied]>=0){(void)dup2(saved[applied],targets[applied]);(void)close(saved[applied]);}
        else(void)close(targets[applied]);
        while(applied){applied--;if(saved[applied]>=0){(void)dup2(saved[applied],targets[applied]);(void)close(saved[applied]);}else(void)close(targets[applied]);}
        return errno;
    }
    result = bk_spawn(path, command[0] ? command : (const char *)0);
    while(applied){applied--;if(saved[applied]>=0){(void)dup2(saved[applied],targets[applied]);(void)close(saved[applied]);}else(void)close(targets[applied]);}
    if (result < 0) return -result;
    *pid = (pid_t)result;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attributes,
                char *const argv[], char *const envp[]) {
    return spawn_common(pid, path, actions, attributes, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes,
                 char *const argv[], char *const envp[]) {
    char path[260];
    const char *directories[] = { "/SYSTEM/PROGRAMS/", "/SYSTEM/COMMANDS/" };
    if (!file || !file[0]) return ENOENT;
    if (strchr(file, '/') || strchr(file, '\\'))
        return spawn_common(pid, file, actions, attributes, argv, envp);
    for (unsigned int i = 0U; i < 2U; i++) {
        unsigned int base = (unsigned int)strlen(directories[i]);
        unsigned int name = (unsigned int)strlen(file);
        if (base + name + 5U >= sizeof(path)) continue;
        strcpy(path, directories[i]);
        strcat(path, file);
        if (access(path, F_OK) == 0)
            return spawn_common(pid, path, actions, attributes, argv, envp);
        if (!strchr(file, '.')) {
            strcat(path, ".BEX");
            if (access(path, F_OK) == 0)
                return spawn_common(pid, path, actions, attributes, argv, envp);
        }
    }
    return ENOENT;
}

int posix_spawnattr_init(posix_spawnattr_t *attributes) {
    if (!attributes) return EINVAL;
    attributes->flags = 0U;
    return 0;
}
int posix_spawnattr_destroy(posix_spawnattr_t *attributes) {
    return attributes ? 0 : EINVAL;
}
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions) {
    if (!actions) return EINVAL;
    actions->action_count = 0U;
    return 0;
}
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions) {
    return actions ? 0 : EINVAL;
}
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions,
                                      int fd) {
    if (!actions) return EINVAL;
    if(actions->action_count>=POSIX_SPAWN_ACTION_MAX)return ENOMEM;
    actions->actions[actions->action_count].type=1;
    actions->actions[actions->action_count].fd=fd;
    actions->action_count++;
    return 0;
}
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions,
                                     int old_fd, int new_fd) {
    if (!actions) return EINVAL;
    if(actions->action_count>=POSIX_SPAWN_ACTION_MAX)return ENOMEM;
    actions->actions[actions->action_count].type=2;
    actions->actions[actions->action_count].fd=old_fd;
    actions->actions[actions->action_count].new_fd=new_fd;
    actions->action_count++;
    return 0;
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
                                     int fd, const char *path, int flags,
                                     mode_t mode) {
    if (!actions||!path) return EINVAL;
    if(actions->action_count>=POSIX_SPAWN_ACTION_MAX)return ENOMEM;
    if(strlen(path)>=sizeof(actions->actions[0].path))return ENAMETOOLONG;
    actions->actions[actions->action_count].type=3;
    actions->actions[actions->action_count].fd=fd;
    actions->actions[actions->action_count].flags=flags;
    actions->actions[actions->action_count].mode=mode;
    strcpy(actions->actions[actions->action_count].path,path);
    actions->action_count++;
    return 0;
}

static int exec_finish(int spawn_result,pid_t child){int status=0;if(spawn_result){errno=spawn_result;return-1;}if(waitpid(child,&status,0)<0)return-1;_exit(WEXITSTATUS(status));}
int execv(const char*path,char*const argv[]){pid_t child;int result=posix_spawn(&child,path,0,0,argv,0);return exec_finish(result,child);}
int execve(const char*path,char*const argv[],char*const envp[]){pid_t child;int result=posix_spawn(&child,path,0,0,argv,envp);return exec_finish(result,child);}
int execvp(const char*file,char*const argv[]){pid_t child;int result=posix_spawnp(&child,file,0,0,argv,0);return exec_finish(result,child);}
static int execl_common(const char*path,const char*arg0,va_list arguments,int search){char*argv[64];pid_t child;int count=0,result;argv[count++]=(char*)arg0;while(count<63&&(argv[count]=va_arg(arguments,char*))!=0)count++;argv[count]=0;result=search?posix_spawnp(&child,path,0,0,argv,0):posix_spawn(&child,path,0,0,argv,0);return exec_finish(result,child);}
int execl(const char*path,const char*arg0,...){int result;va_list arguments;va_start(arguments,arg0);result=execl_common(path,arg0,arguments,0);va_end(arguments);return result;}
int execlp(const char*file,const char*arg0,...){int result;va_list arguments;va_start(arguments,arg0);result=execl_common(file,arg0,arguments,1);va_end(arguments);return result;}
int system(const char*command){char*argv[4];pid_t child;int status,result;if(!command)return access("/SYSTEM/COMMANDS/SH.BEX",F_OK)==0;argv[0]=(char*)"sh";argv[1]=(char*)"-c";argv[2]=(char*)command;argv[3]=0;result=posix_spawn(&child,"/SYSTEM/COMMANDS/SH.BEX",0,0,argv,0);if(result){errno=result;return-1;}if(waitpid(child,&status,0)<0)return-1;return status;}
