/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <fcntl.h>
#include "spawn.h"
#include <readline/readline.h>
#include <readline/history.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

static void handle_child_status(pid_t pid, int status);
extern char **environ;

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */

    /* Add additional fields here if needed. */

    int pid;
    int numChildren; 
    int pgid;
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Done";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}


/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));


    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;


        //waits for next child porcess to die, when you pass -1 it just waits for the next process
        //waitpid returns status
        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else{
            utils_fatal_error("waitpid is failed");
        }
    }
}


static void
handle_child_status(pid_t pid, int status)
{
     assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */

    struct job * currentJob = NULL;
    struct list_elem * e = list_begin(&job_list);
    
    for(; e != list_end(&job_list); e = list_next(e)){
        currentJob = list_entry(e, struct job, elem);

        if(currentJob->pid == pid){
            break;
        }
    }

    if (WIFEXITED(status))
    {   
        int statusCode = WEXITSTATUS(status);
        currentJob->num_processes_alive--;

        if(statusCode == 0 && currentJob->status==FOREGROUND){
            termstate_sample();
        }
    }

    if (WIFSIGNALED(status))
    {
        //Cases:
        //Ctrl-C 
        if (WTERMSIG(status) == SIGINT)
        {
            currentJob->status = FOREGROUND;
            currentJob->num_processes_alive--;
            printf("%s\n", strsignal(WTERMSIG(status)));
        }

        //User terminate process with kill
        else if (WTERMSIG(status) == SIGTERM)
        {
            currentJob->num_processes_alive--;
            printf("%s\n", strsignal(WTERMSIG(status)));
        }

        //User terminate process with kill -9
        else if (WTERMSIG(status) == SIGKILL)
        {
            currentJob->num_processes_alive--;
            printf("%s\n", strsignal(WTERMSIG(status)));
        }

        //Process has been terminated, general case
        else
        {   
            currentJob->num_processes_alive--;
            printf("%s\n", strsignal(WTERMSIG(status)));
        }
    }

    if (WIFSTOPPED(status))
    {
        termstate_save(&currentJob->saved_tty_state);
        //Cases:
        //Ctrl-Z
        if (WSTOPSIG(status) == SIGTSTP)
        {
            currentJob->status = STOPPED;
            print_job(currentJob);
        }

        //User stop process with kill -STOP
        else if (WSTOPSIG(status) == SIGSTOP)
        {
            currentJob->status = STOPPED;
        }

        //Non foreground process wants terminal access
        else if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN)
        {
            currentJob->status = NEEDSTERMINAL;
        }
    }
}

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    //Reads through one line which is your command line element 
    for (;;) {
        

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        struct ast_command_line * cline = ast_parse_command_line(cmdline);
        // free (cmdline);
        if (cline == NULL)                  /* Error in command line */
            continue;

        //History implementation
        char* history;
        int res = history_expand(cmdline, &history);
       
        if(res == -1){
            free(history);
        }
        else{
             add_history(history);
        }

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }
     
       

        //Loop to check any job with num process alive with 0, and then delete under here
        struct job * delJob = NULL;
        struct list_elem * e = list_begin(&job_list);
        
        for(; e != list_end(&job_list); e = list_next(e)){
            struct job *tempJob = list_entry(e, struct job, elem);
            if(tempJob->num_processes_alive == 0){
                delJob = tempJob;
                list_remove(e);
                break;
            }
        }

        if(delJob != NULL){
            delete_job(delJob);
        }

        //ast_command_line_print(cline);      /* Output a representation of
        //                                       the entered command line */
        struct list_elem* currPipeNode;

        for(currPipeNode = list_begin(&cline->pipes); currPipeNode != list_end(&cline->pipes); currPipeNode=list_next(currPipeNode)){

        //Get ast pipeline element using the command line from ^ step
        struct ast_pipeline* currPipe = list_entry(currPipeNode, struct ast_pipeline, elem);
        struct list_elem* firstPipeline = list_begin(&currPipe->commands);
        //Get pipe-element 
        struct ast_command* currCmd = list_entry(firstPipeline, struct ast_command, elem);

        int listSize = list_size(&currPipe->commands);
        struct job* currentJob = NULL;

        //Each of these commands looks for processes/commands inside of the job
        if (strcmp(currCmd->argv[0], "jobs") == 0){
            
            struct list_elem * e = list_begin(&job_list);
    
            for(; e != list_end(&job_list); e = list_next(e))
            {
                currentJob = list_entry(e, struct job, elem);
                print_job(currentJob);
            }
        }

        else if (strcmp(currCmd->argv[0], "fg") == 0){

            //fg <job id> syntax
            int jobID = atoi(currCmd->argv[1]);
            currentJob = get_job_from_jid(jobID);
            currentJob->pipe->bg_job = false;
            currentJob->status = FOREGROUND;
            print_cmdline(currentJob->pipe);
            printf("\n");
         
            termstate_give_terminal_to(&currentJob->saved_tty_state, currentJob->pgid);
            killpg(currentJob->pgid, SIGCONT);
            signal_block(SIGCHLD);
            wait_for_job(currentJob);
            signal_unblock(SIGCHLD);
        }


        else if (strcmp(currCmd->argv[0], "bg") == 0){
            int jobID = atoi(currCmd->argv[1]);
            currentJob = get_job_from_jid(jobID);
            currentJob->pipe->bg_job = true;
            currentJob->status = BACKGROUND;
            print_cmdline(currentJob->pipe);
            printf("\n");

            termstate_give_terminal_back_to_shell();
            killpg(currentJob->pgid, SIGCONT);
        }

        else if (strcmp(currCmd->argv[0], "kill") == 0){
            int jobID = atoi(currCmd->argv[1]);
            currentJob = get_job_from_jid(jobID);
            killpg(currentJob->pgid, SIGTERM);
        }

        else if (strcmp(currCmd->argv[0], "exit") == 0){
            exit(0);
        }

        else if (strcmp(currCmd->argv[0], "stop") == 0){

            int jobID = atoi(currCmd->argv[1]);
            currentJob = get_job_from_jid(jobID);
            print_job(currentJob);

            killpg(currentJob->pgid, SIGTSTP);
        }

        //cd built in 
        else if (strcmp(currCmd->argv[0], "cd") == 0){
            char *dir;
            if (currCmd->argv[1] == NULL) {
                dir = getenv("HOME");
            } else {
                dir = currCmd->argv[1];
            }
            if (chdir(dir) != 0) {
                utils_error("No such file or directory\n");
            }
        }
        //history built in
        else if(strcmp(currCmd->argv[0], "history") == 0){
            using_history();
            HIST_ENTRY **historyL = history_list();
            for(int i = 0; i < history_length; i++){
                printf("   %d %s\n", i + history_base, historyL[i]->line);
            }

        }

        else{

            if(currentJob == NULL)
            {
                currentJob = add_job(currPipe);
                currentJob->numChildren = 0;
            }
            
            struct list_elem * e = list_begin (&currPipe->commands); 

            int commandsLeft = listSize;
            //Initialize file descriptor for first pipe
            int firstPipeEnds[2];
            int i = 0;

            //This is the for loop through the pipeline
            for (; e != list_end (&currPipe->commands); e = list_next(e)) {
                struct ast_command *currCmd = list_entry(e, struct ast_command, elem);
                commandsLeft--;
                i++;

                posix_spawn_file_actions_t child_file_attr;
                posix_spawnattr_t child_spawn_attr; 

                posix_spawnattr_init(&child_spawn_attr);
                posix_spawn_file_actions_init(&child_file_attr);
            
                //Set process groups
                if(currentJob->numChildren == 0){
                    posix_spawnattr_setpgroup(&child_spawn_attr, 0);
                    posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP);
                }
                else{
                    posix_spawnattr_setpgroup(&child_spawn_attr, currentJob->pgid);
                    posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP);
                }
                
                //FOREGROUND
                if (!currPipe->bg_job) 
                {
                    currentJob->status = FOREGROUND;
                    posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd());
                    posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_TCSETPGROUP| POSIX_SPAWN_SETPGROUP);
                }
                else
                {   
                    currentJob->status = BACKGROUND;
                    posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd());
                    posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP);
                }

                //IO Redirection 
                 if (currPipe->iored_input != NULL && e == list_begin(&currPipe->commands))//first command
                {
                    posix_spawn_file_actions_addopen(&child_file_attr, 0, currPipe->iored_input, O_RDONLY, S_IRWXU | S_IRWXG | S_IRWXO);
                }
                if (currPipe->iored_output != NULL && list_next(e) == list_end(&currPipe->commands)) //last command
                {
                    int flag = O_CREAT | O_WRONLY;
                    if (currPipe->append_to_output) {
                        flag |= O_APPEND;
                    } else {
                        flag |= O_TRUNC;
                    }
                    posix_spawn_file_actions_addopen(&child_file_attr, 1, currPipe->iored_output, flag, S_IRWXU | S_IRWXG | S_IRWXO);
                }
                
                //Initialize second 2D pipe array
                int secondPipeEnds[2]; 
                //More than one command
                if (listSize > 1)
                {
                    // Piping implementation
                    if (e == list_begin(&currPipe->commands))
                    {   
                        pipe2(firstPipeEnds, O_CLOEXEC);
                        posix_spawn_file_actions_adddup2(&child_file_attr, firstPipeEnds[1], 1);
                    }
                    else if (list_next(e) == list_end(&currPipe->commands)) 
                    {
                        posix_spawn_file_actions_adddup2(&child_file_attr, firstPipeEnds[0], 0);
                    } 
                    else
                    {
                        pipe2(secondPipeEnds, O_CLOEXEC);
                        posix_spawn_file_actions_adddup2(&child_file_attr, firstPipeEnds[0], 0);
                        posix_spawn_file_actions_adddup2(&child_file_attr, secondPipeEnds[1], 1);
                    }            
                }

                if (currCmd->dup_stderr_to_stdout)
                {
                    posix_spawn_file_actions_adddup2(&child_file_attr, 1, 2);
                }

                int pid;
                int spawned = posix_spawnp(&pid,  currCmd->argv[0],&child_file_attr, &child_spawn_attr, currCmd->argv, environ);

                //Need to close pipes
                if (listSize > 1)
                {
                    //First process, list begin
                    if(i == 1)
                    {
                        close(firstPipeEnds[1]);
                    }
                    //Middle process
                    if((i != 1) & (i != listSize)){
                        close(secondPipeEnds[1]);
                        close(firstPipeEnds[0]);
                        firstPipeEnds[0] = secondPipeEnds[0];
                    }
                    //Last process, list end
                    if(i == listSize){
                        close(firstPipeEnds[0]);
                    }
                }
                //Check if posix spawn return 0
                if(spawned == 0){

                    if(currentJob->numChildren == 0){
                        currentJob->pgid = pid;
                        currentJob->pid = pid;
                    }

                    currentJob->numChildren++;
                    currentJob->num_processes_alive++;
                    if (currPipe->bg_job)
                    {
                        fprintf(stderr, "[%d] %d\n", currentJob->jid, currentJob->pgid);
                        termstate_save(&currentJob->saved_tty_state);
                    }
                    
                }
                //Invalid command, delete from job list
                else{
                    struct job * delJob = NULL;
                    struct list_elem * e = list_begin(&job_list);

                    for(; e != list_end(&job_list); e = list_next(e)){
                        struct job *tempJob = list_entry(e, struct job, elem);
                        if(tempJob->pid == currentJob->pid){
                            delJob = tempJob;
                            list_remove(e);
                            break;
                        }
                    }
                    if(delJob != NULL){
                        delete_job(delJob);
                    }
                    utils_error("No such file or directory\n");
                }
               
            }
                if (!currPipe->bg_job)
                {

                    signal_block(SIGCHLD);
                    wait_for_job(currentJob);
                    signal_unblock(SIGCHLD);
                }
            
        }

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_picpelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */

        //ast_command_line_free(cline);
        termstate_give_terminal_back_to_shell();

        }

    }

    return 0;
}


