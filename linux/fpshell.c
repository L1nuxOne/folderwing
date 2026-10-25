/* Bash with the user's rc file and OpenPilot prompt/cwd markers. The path
 * arrives as an argv value; no user filename becomes shell source code. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
char *fp_windows_path(const char *path);
int fp_shell_main(const char *path)
{
    size_t len=strlen(path);char *decoded=NULL;
    if(len<3 || (path[0]!='x' && path[0]!='w') || (len-1)%2)return 1;
    decoded=malloc((len-1)/2+1);if(!decoded)return 1;
    for(size_t i=1;i<len;i+=2){
        unsigned a,b;
        if(sscanf(path+i,"%1x%1x",&a,&b)!=2 || !(a*16+b)){free(decoded);return 1;}
        decoded[(i-1)/2]=(char)(a*16+b);
    }
    decoded[(len-1)/2]=0;
    char *dir=path[0]=='x'?strdup(decoded):fp_windows_path(decoded);free(decoded);
    if(!dir || chdir(dir)){
        fprintf(stderr,"OpenPilot: could not enter the terminal folder: %s\n",strerror(errno));free(dir);return 1;
    }
    free(dir);
    char rc[]="/tmp/openpilot-bash-XXXXXX";int fd=mkstemp(rc);
    if(fd<0){perror("OpenPilot: shell setup");return 1;}
    FILE *file=fdopen(fd,"w");if(!file){close(fd);unlink(rc);return 1;}
    fprintf(file,"command rm -- '%s'\n",rc); /* the already-open rc stream remains readable */
    fputs("if [[ -r $HOME/.bashrc ]]; then source \"$HOME/.bashrc\"; fi\n"
          "__openpilot_prompt() { local LC_ALL=C __hex='' __char __i; "
          "for ((__i=0;__i<${#PWD};__i++)); do builtin printf -v __char '%02x' \"'${PWD:__i:1}\"; __hex+=$__char; done; "
          "builtin printf '\\033]133;D\\007\\033]133;A\\007\\033]9;9;wsl:%s;%s\\007' \"$WSL_DISTRO_NAME\" \"$__hex\"; [[ $PS1 == *'\\[\\e]133;B\\a\\]' ]] || PS1+='\\[\\e]133;B\\a\\]'; }\n"
          "PROMPT_COMMAND=(\"${PROMPT_COMMAND[@]}\" __openpilot_prompt)\n"
          "PS1+=\"\\[\\e]133;B\\a\\]\"\n",file);
    int failed=ferror(file);if(fclose(file))failed=1;
    if(failed){unlink(rc);return 1;}
    pid_t pid=fork();
    if(!pid){execl("/bin/bash","bash","--rcfile",rc,"-i",(char *)NULL);perror("OpenPilot: bash");_exit(127);}
    if(pid<0){unlink(rc);return 1;}
    int status=0;while(waitpid(pid,&status,0)<0){if(errno==EINTR)continue;unlink(rc);return 1;}
    unlink(rc);
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
