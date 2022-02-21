/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <spawn.h>
#include <time.h>
#include <fcntl.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

static void handle_child_status(pid_t pid, int status);

static int runBuiltIn(struct ast_pipeline *currpipeline);

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
build_prompt(int *command)
{

    return strdup("cush> ");
}

enum job_status
{
    FOREGROUND,    /* job is running in foreground.  Only one job can be
                      in the foreground state. */
    BACKGROUND,    /* job is running in background */
    STOPPED,       /* job is stopped via SIGSTOP */
    NEEDSTERMINAL, /* job is stopped because it was a background job
                      and requires exclusive terminal access */
    DONE           /* the job that is finished */
};

struct job
{
    struct list_elem elem;          /* Link element for jobs list. */
    struct ast_pipeline *pipe;      /* The pipeline of commands this job represents */
    int jid;                        /* Job id. */
    enum job_status status;         /* Job status. */
    int num_processes_alive;        /* The number of processes that we know to be alive */
    struct termios saved_tty_state; /* The state of the terminal when this job was
                                       stopped after having been in foreground */

    /* Add additional fields here if needed. */
    pid_t pgid;
};

/* Utility functions for job list management.
 * We use 2 data structures:
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1 << 16)
static struct list job_list;

static struct job *jid2job[MAXJOBS];

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
    struct job *job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);

    job->jid = 0;
    if (pipe->bg_job)
    {
        job->status = BACKGROUND;
    }

    for (int i = 1; i < MAXJOBS; i++)
    {
        if (jid2job[i] == NULL)
        {
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

static void
remove_from_list(struct job *job)
{
    list_remove(&job->elem);
    delete_job(job);
}

/* Signal Handler for SIGINT */
static void sigintHandler(int sig_num)
{
    signal(SIGINT, sigintHandler);
}

static const char *
get_status(enum job_status status)
{
    switch (status)
    {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    case DONE:
        return "Done";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem *e = list_begin(&pipeline->commands);
    for (; e != list_end(&pipeline->commands); e = list_next(e))
    {
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

    while ((child = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0)
    {
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

    while (job->status == FOREGROUND && job->num_processes_alive > 0)
    {
        int status;

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
        else
            utils_fatal_error("waitpid failed, see code for explanation");
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
    if (pid > 0)
    {

        struct job *job = NULL; // We are available to set a struct type that is
                                // NULL initially

        // We can take advantage of methods in list.h to solve
        // the problem efficiently.

        int found = -1; // the initial value of job pid.

        for (struct list_elem *e = list_begin(&job_list);
             e != list_end(&job_list);
             e = list_next(e))
        {

            // We can access the element with respect to list_entry
            job = list_entry(e, struct job, elem);

            struct ast_command *com = NULL;
            // use a for loop to iterate every process in one job
            for (struct list_elem *p = list_begin(&job->pipe->commands);
                 p != list_end(&job->pipe->commands); p = list_next(p))
            {

                com = list_entry(p, struct ast_command, elem);
                // If their pid correspond to one another, we may break
                // the for loop and use the current job.

                if (com->pid == pid)
                {
                    found = job->jid;
                    break;
                }
            }

            if (found != -1)
            {
                break;
            }

            // Otherwise, we want to set job into NULL.
            job = NULL;
        }

        if (job == NULL)
        {
            // If the job does not have a corresponding pid as the parameter pid did,
            // we will receive a fatal error
            utils_fatal_error("Error There are no current jobs received from the signal.");
        }
        else
        {

            if (WIFEXITED(status))
            {
                // What happen if the program is exited.
                // We decrement the variable number_of_processes alive in the job.
                job->num_processes_alive--;
                if (job->num_processes_alive == 0)
                {
                    job->status = DONE;
                }
            }
            else if (WIFSIGNALED(status))
            {
                // What happen if the child process received a signal that is terminated.
                // We receive a number that terminates the signal.
                int terNum = WTERMSIG(status);

                // Later, the terNum can be divided into several scenarios: aborted, floating
                // pointer exception, killed, segmentation fault, and terminated.
                if (terNum == 6)
                {
                    // The program is aborted.
                    utils_error("aborted\n");
                }
                else if (terNum == 8)
                {
                    // The program encounters a floating point exception.
                    utils_error("floating point exception\n");
                }
                else if (terNum == 9)
                {
                    // a killed signal errors.
                    utils_error("killed\n");
                }
                else if (terNum == 11)
                {
                    // The segmentation fault is more likely to produce.
                    utils_error("segmentation fault\n");
                }
                else if (terNum == 15)
                {
                    // The process is terminated.
                    utils_error("terminated\n");
                }

                // The number of processes that is alive decreases.
                job->num_processes_alive--;
            }
            else if (WIFSTOPPED(status))
            {
                // What happen if the child process receives a signal that is stopped.

                // The status of the job must be set STOPPED in the enumerator job_status by default.
                job->status = STOPPED;
                termstate_save(&job->saved_tty_state);

                // Later, the status of the process may be modified automatically.
                int stpNum = WSTOPSIG(status);

                // The background stage can be depicted in two possibiltiies: (1) stpNum == SIGTTOU
                // | stpNum == SIGTTIN. (2) we print the job.
                if (stpNum == SIGTTOU || stpNum == SIGTTIN)
                {
                    // If the stpNum is stopped, the job's status will need the terminal.
                    job->status = NEEDSTERMINAL;
                }
                else
                {
                    print_job(job);
                }
            }
        }
    }
    else
    {
        utils_fatal_error("Error in waiting for signal from the child process");
    }
    termstate_give_terminal_back_to_shell();
}

static void execute(struct ast_pipeline *currpipeline)
{
    // We would like to add jobs to the current pipeline
    struct job *job = add_job(currpipeline);

    int size = list_size(&currpipeline->commands);

    int pipes[size][2]; // read and write
                        // 0 stands for read
                        // 1 stands for write

    for (int i = 0; i < size + 1; i++)
    {
        pipe2(pipes[i], O_CLOEXEC);
    }

    // // I am going to deal with the input with its corresponding
    // // file descriptor
    // int inputfd = -1;

    // while(currpipeline->iored_input != NULL) {
    //     inputfd = open(currpipeline->iored_input, O_RDONLY);
    // }

    // // I am going to deal with the output with its corresponding
    // // file descriptor.
    // int outputfd = -1;

    // while(currpipeline->iored_output != NULL) {

    //     if (currpipeline->append_to_output) {
    //         outputfd = open(currpipeline->iored_output, O_WRONLY|O_CREAT|O_APPEND);
    //     }
    //     else {
    //         outputfd = open(currpipeline->iored_output, O_WRONLY|O_CREAT);
    //     }
    // }

    signal_block(SIGCHLD);
    // int inputfd = -1;
    // int outputfd = -1;

    int commndNum = 0;
    int success = -1; // the child process
    for (struct list_elem *e = list_begin(&currpipeline->commands); e != list_end(&currpipeline->commands);
         e = list_next(e))
    {
        pid_t child; // a child is established.
        posix_spawn_file_actions_t file_actions;
        posix_spawn_file_actions_init(&file_actions);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);

        if (currpipeline->bg_job)
        {
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        }
        else
        {
            job->status = FOREGROUND;
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_TCSETPGROUP | POSIX_SPAWN_SETPGROUP);
            posix_spawnattr_tcsetpgrp_np(&attr, termstate_get_tty_fd());
        }
        if (list_begin(&currpipeline->commands))
        {
            posix_spawnattr_setpgroup(&attr, 0);
        }
        else
        {
            posix_spawnattr_setpgroup(&attr, job->pgid);
        }

        struct ast_command *command = list_entry(e, struct ast_command, elem);

        // addopen(open)
        if (list_begin(&currpipeline->commands))
        {
            if (currpipeline->iored_input)
            {
                posix_spawn_file_actions_addopen(&file_actions, 0, currpipeline->iored_input, O_RDONLY, 0);
            }
        }

        if (list_end(&currpipeline->commands))
        {
            if (currpipeline->iored_output)
            {
                if (currpipeline->append_to_output)
                {
                    posix_spawn_file_actions_addopen(&file_actions, 1, currpipeline->iored_output, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU);
                }
                else
                {
                    posix_spawn_file_actions_addopen(&file_actions, 1, currpipeline->iored_output, O_WRONLY | O_CREAT, S_IRWXU);
                }
            }
        }

        if (command->dup_stderr_to_stdout)
        {
            posix_spawn_file_actions_adddup2(&file_actions, 2, 1);
        }

        // piping(dup2)
        //  (1) check it is the first and the last
        //  (2) check it is the first
        //  (3) check it is the last
        //  (4) check it is the middle

        if (list_size(&currpipeline->commands) > 1)
        {
            if (list_begin(&currpipeline->commands))
            {
                // the first command: stdin
                posix_spawn_file_actions_adddup2(&file_actions, 0, STDIN_FILENO);
            }
            else if (list_end(&currpipeline->commands))
            {
                // the last command: stdout
                posix_spawn_file_actions_adddup2(&file_actions, 1, STDOUT_FILENO);
            }
            else
            {
                // the middle command: stdin and stdout
                posix_spawn_file_actions_adddup2(&file_actions, 0, STDIN_FILENO);
                posix_spawn_file_actions_adddup2(&file_actions, 1, STDOUT_FILENO);
            }
        }

        // This is the scenario used to handle the child status.
        success = posix_spawnp(&child, command->argv[0], &file_actions, &attr, command->argv, environ);
        if (success != 0)
        {
            fprintf(stderr, "no such file or directory\n");
            break;
        }
        command->pid = child;

        // The process group id is supposed to be the first process id.
        if (e == list_begin(&currpipeline->commands))
        {
            job->pgid = command->pid;
        }

        if (currpipeline->bg_job)
        {
            printf("[%d] %d\n", job->jid, command->pid);
        }

        job->num_processes_alive++;
        commndNum++;
    }

    // The parent process pid
    for (int i = 0; i < size; i++)
    {
        if (i == 0)
        {
            // For parent, the input file descriptor is addopen().
            if (STDIN_FILENO > 0)
            {
                close(pipes[i][0]);
            }
            else
            {
                close(pipes[i][0]);
                close(pipes[i][1]);
            }
        }
        else
        {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
    }

    // termstate_save(&job->saved_tty_state);

    if (success == 0)
    {
        wait_for_job(job);
        signal_unblock(SIGCHLD);
    }
    else
    {
        remove_from_list(job);
        termstate_give_terminal_back_to_shell();
        signal_unblock(SIGCHLD);
        return;
    }

    /* if (job -> status == FOREGROUND) {
        termstate_give_terminal_to(&job->saved_tty_state, job->pgid);
        wait_for_job(job);
    }
 */
    if (job->status == BACKGROUND)
    {
        printf("[%d] %d\n", job->jid, job->pgid);
    }

    if (job->status == DONE)
    {
        print_job(job);
        remove_from_list(job);
    }

    // termstate_give_terminal_back_to_shell();

    // signal_unblock(SIGCHLD);
}

static int runBuiltIn(struct ast_pipeline *currpipeline)
{
    struct ast_command *command = list_entry(list_begin(&currpipeline->commands), struct ast_command, elem);
    char **argv = command->argv; // the argument array
    int argc = 0;                // the number of arguments in a command

    while (*(argv + argc) != NULL)
    {
        argc++;
    }

    if (strcmp(argv[0], "kill") == 0)
    {
        // kill
        if (argc == 2)
        {
            // the job id for kill is obtained
            int jidforKill = atoi(*(argv + 1));

            // we can get the job from the corresponding id
            struct job *killJob = get_job_from_jid(jidforKill);

            if (killJob == NULL)
            {
                // If the job was not found, we will return the statement below.
                printf("kill %d: no such job\n", jidforKill);
            }
            else
            {

                // If the job was found, a signal can be set.
                int status = killpg(killJob->pgid, SIGTERM);

                if (status == 0)
                {
                    // If the status succeeds, we can remove it from the list.
                    waitpid(killJob->pgid, &status, WUNTRACED);
                    remove_from_list(killJob);
                }
                else
                {
                    // The signal is failed.
                    printf("Kill on job: %d was unsuccessful\n", jidforKill);
                }
            }
        }
        else
        {
            printf("Incorrect number of arguments for the command 'kill'\n");
        }
        return 1;
    }
    else if (strcmp(argv[0], "fg") == 0)
    {
        signal_block(SIGCHLD);
        // fg
        struct job *fgJob = NULL; // the job for fg
        int jidforFg = 0;         // the job id for fg

        if (argc == 1)
        {
            printf("fg: job id missing\n");
            return 1;
        }
        else if (argc == 2)
        {
            jidforFg = atoi(argv[1]);
            fgJob = get_job_from_jid(jidforFg);

            if (fgJob == NULL)
            {
                printf("fg: %d: No such job\n", jidforFg);
                return 1;
            }
            if (fgJob->status == FOREGROUND)
            {
                printf("Job: %d is already running\n", jidforFg);
                return 1;
            }
        }
        else
        {
            printf("Incorrect number of arguments for the command 'fg'\n");
        }

        struct ast_pipeline *pipe = fgJob->pipe;
        command = list_entry(list_begin(&pipe->commands), struct ast_command, elem);
        // The job was found
        int status = killpg(fgJob->pgid, SIGCONT); // the signal we are available to use in the command fg
                                                   // is SIGCONT

        if (status == 0)
        {

            termstate_give_terminal_to(NULL, fgJob->pgid);
            fgJob->status = FOREGROUND; // the status of the job can be switched into FOREGROUND
            print_job(fgJob);           // print the job
            wait_for_job(fgJob);        // wait for the job to complete other processes
            signal_unblock(SIGCHLD);
            if (fgJob->status == FOREGROUND)
            {
                remove_from_list(fgJob);
            }
        }
        else
        {
            printf("fg on job: %d was unsuccessful\n", jidforFg);
        }
        termstate_give_terminal_back_to_shell();
        return 1;
    }
    else if (strcmp(argv[0], "bg") == 0)
    {
        // bg

        struct job *bgJob = NULL; // bgJob must be initialized
        int jidforBg = 0;         // the job id for the command bg

        if (argc == 1)
        {
            printf("bg: job id missing\n");
            return 1;
        }
        else
        {
            jidforBg = atoi(argv[1]);
            bgJob = get_job_from_jid(jidforBg);

            if (bgJob == NULL)
            {
                printf("bg %d: No such job\n", jidforBg);
                return 1;
            }
            else if (bgJob->status != STOPPED)
            {
                printf("bg: %d is already in background\n", jidforBg);
                return 1;
            }
        }

        struct ast_pipeline *pipe = bgJob->pipe;
        command = list_entry(list_begin(&pipe->commands), struct ast_command, elem);
        int status = killpg(bgJob->pgid, SIGCONT); // Similar to what we
                                                   // have done before,
                                                   // the signal should
                                                   // be set to SIGCONT;
        if (status == 0)
        {
            // The signal is valid.
            bgJob->status = BACKGROUND; // It enters the background stage.
            signal_unblock(SIGCHLD);
            print_job(bgJob);
        }
        else
        {
            printf("bg on job: %d was unsuccessful\n", jidforBg);
        }
        termstate_give_terminal_back_to_shell();

        return 1;
    }
    else if (strcmp(argv[0], "jobs") == 0)
    {
        // jobs
        signal_block(SIGCHLD);
        if (argc == 1)
        {
            if (!list_empty(&job_list))
            {
                for (struct list_elem *e = list_begin(&job_list);
                     e != list_end(&job_list); e = list_next(e))
                {
                    // We basically use a for loop to keep track of each job in the
                    // job list.
                    struct job *currJob = list_entry(e, struct job, elem);

                    print_job(currJob);
                    if (currJob->status == DONE)
                    {
                        e = list_prev(e);
                        remove_from_list(currJob);
                    }
                }
            }
            else
            {
                printf("There are currently not jobs in the job list.\n");
            }
        }
        else
        {
            printf("Incorrect number of arguments for the command jobs\n");
        }
        return 1;
    }
    else if (strcmp(argv[0], "stop") == 0)
    {
        // stop

        if (argc == 2)
        {
            // It is similar to kill command above

            int jidforStop = atoi(argv[1]);                        // We may get the corresponding
                                                                   // jid for the stop command
            struct job *jobforStop = get_job_from_jid(jidforStop); // the job is
                                                                   // obtained.

            if (jobforStop == NULL)
            {
                printf("stop %d: No such job\n", jidforStop);
            }
            else
            {

                struct ast_pipeline *pipe = jobforStop->pipe;
                command = list_entry(list_begin(&pipe->commands), struct ast_command, elem);

                int status = killpg(jobforStop->pgid, SIGSTOP); // The signal can be
                                                                // set as stop
                if (status == 0)
                {
                    jobforStop->status = STOPPED;                 // The status should
                                                                  // be regarded as stop
                    termstate_save(&jobforStop->saved_tty_state); // the state of
                                                                  // terminal is saved.
                }
                else
                {
                    printf("Stop on job: %d was unsuccessful\n", jidforStop);
                }
            }
        }
        else
        {
            printf("Incorrect number of arguments for command 'stop'\n");
        }
        return 1;
    }
    else if (strcmp(argv[0], "exit") == 0)
    {
        // exit
        exit(0);
    }

    return 0;
}

int main(int ac, char *av[])
{
    int opt;
    signal(SIGINT, sigintHandler);

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0)
    // We would like to determine whether or not
    // the option is available to be used.
    {
        switch (opt)
        {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    int num_com = 0;

    /* Read/eval loop. */
    for (;;)
    {

        /* Do not output a prompt unless shell's stdin is a terminal */

        // getPath();
        char *prompt = isatty(0) ? build_prompt(&num_com) : NULL;
        char *cmdline = readline(prompt);
        free(prompt);

        if (cmdline == NULL) /* User typed EOF */
            break;

        struct ast_command_line *cline = ast_parse_command_line(cmdline); // We would like to parse
                                                                          // each job, where
                                                                          // job contains multiple
                                                                          // pipelines.
        free(cmdline);                                                    // We would like to
        if (cline == NULL)                                                /* Error in command line */
            // If something goes wrong with pipeline, what are we supposed
            // to do
            continue;

        if (list_empty(&cline->pipes))
        { /* User hit enter */
            // If the command line does not contain pipelines, we
            // will be ready to free it.
            ast_command_line_free(cline);
            continue;
        }
        // ast_command_line_print(cline); /* Output a representation of
        /* the entered command line */
        // We may focus on each pipeline
        for (struct list_elem *e = list_begin(&cline->pipes);
             e != list_end(&cline->pipes);
             e = list_next(e))
        {
            // We deal with pipe one-by-one.
            struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
            if (!runBuiltIn(pipe))
            {
                execute(pipe);
            }
        }
        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        // ast_command_line_free(cline);
        free(cline);
    }
    return 0;
}
