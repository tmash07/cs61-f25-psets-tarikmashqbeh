#include "sh61.hh"
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>

// For the love of God
#undef exit
#define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__


struct redirection {
    int fd;               
    std::string filename;
    bool is_input;
};

struct command {
    std::vector<std::string> args;
    std::vector<redirection> redirs;
    pid_t pid = -1;

    command();
    ~command();

    void run(int stdin_fd = -1, int stdout_fd = -1);
};

// command::command()
//    This constructor function initializes a `command` structure. You may
//    add stuff to it as you grow the command structure.

command::command() {
}

// command::~command()
//    This destructor function is called to delete a command.

command::~command() {
}


// COMMAND EXECUTION

// Helper command that applies file redirections in a child process before commands run
static void apply_redirections(const std::vector<redirection>& redirs) {
    for (auto const& r : redirs) {
        // Open a file that is read-only for input and write-only for output
        // O_CREAT and O_TRUNC ensure file exists and is empty
        int flags = r.is_input ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);
        int fd = open(r.filename.c_str(), flags, 0666);
        
        // Handle error from making file
        if (fd < 0) {
            perror(r.filename.c_str());
            _exit(1);
        }
        
        // Point r.fd to the newly opened file
        if (dup2(fd, r.fd) < 0) {
            perror("dup2");
            close(fd);
            _exit(1);
        }
        // Close original file
        close(fd); 
    }
}

// command::run()
//    Creates a single child process running the command in `this`, and
//    sets `this->pid` to the pid of the child process.
//
//    If a child process cannot be created, this function should call
//    `_exit(EXIT_FAILURE)` (that is, `_exit(1)`) to exit the containing
//    shell or subshell. If this function returns to its caller,
//    `this->pid > 0` must always hold.
//
//    Note that this function must return to its caller *only* in the parent
//    process. The code that runs in the child process must `execvp` and/or
//    `_exit`.
//
//    PHASE 1: Fork a child process and run the command using `execvp`.
//       This will require creating a vector of `char*` arguments using
//       `this->args[N].c_str()`. Note that the last element of the vector
//       must be a `nullptr`.
//    PHASE 4: Set up a pipeline if appropriate. This may require creating a
//       new pipe (`pipe` system call), and/or replacing the child process's
//       standard input/output with parts of the pipe (`dup2` and `close`).
//       Draw pictures!
//    PHASE 7: Handle redirections.

void command::run(int stdin_fd, int stdout_fd) {
    assert(this->pid == -1);
    
    // Handle empty command
    if (this->args.empty() && this->redirs.empty()) {
        this->pid = 0; 
        return;
    }

    // Build argv
    std::vector<char*> argv;
    argv.reserve(this->args.size() + 1);
    for (auto& s : this->args) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    pid_t p = fork();
    // Handle fork error
    if (p < 0) {
        perror("fork");
        _exit(EXIT_FAILURE);
    } else if (p == 0) {
        // Set up pipeline connections
        if (stdin_fd >= 0) {
            if (dup2(stdin_fd, STDIN_FILENO) < 0) {
                perror("dup2");
                _exit(1);
            }
            close(stdin_fd);
        }
        if (stdout_fd >= 0) {
            if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
                perror("dup2");
                _exit(1);
            }
            close(stdout_fd);
        }
        // Ensure all other pipe file descriptors are closed (1024 is normal limit for open fd)
        for (int fd = 3; fd < 1024; ++fd) {
            close(fd);  
        }

        // Apply redirections
        apply_redirections(this->redirs);

        // Run the command
        if (argv[0] != nullptr) {
            execvp(argv[0], argv.data());
            perror(argv[0]);
            _exit(EXIT_FAILURE);
        } else { // Handle case with no command
            _exit(0);
        }
    }
    // Save child pid
    this->pid = p;
}

// Helper function to parse redirections from command line input
static void parse_redirs(command_parser cmdp, std::vector<std::string>& args, std::vector<redirection>& redirs) {
    args.clear(); 
    redirs.clear();

    bool expect_filename = false;
    int pending_fd = -1;

    for (auto tok = cmdp.token_begin(); tok != cmdp.end(); ++tok) {
        int t = tok.type();

        // If token is a filename
        if (expect_filename) {
            if (t == TYPE_NORMAL) {
                redirection r;
                r.fd = pending_fd;
                r.filename = tok.str();
                r.is_input = (pending_fd == STDIN_FILENO);
                redirs.push_back(r);
                expect_filename = false;
                continue;
            }
        }
        // If token is a redirector 
        if (t == TYPE_REDIRECT_OP) {
            std::string op = tok.str();
            if (op == "<") {
                pending_fd = STDIN_FILENO;
            } else if (op == ">") {
                pending_fd = STDOUT_FILENO;
            } else if (op == "2>") {
                pending_fd = STDERR_FILENO;
            }
            expect_filename = true;
        } else if (t == TYPE_NORMAL) { // If token is a command
            args.push_back(tok.str());
        }
    }
}

// Helper function to make exit codes in waitpid-style (for && and ||)
static int make_status(int code) {
    return code << 8;
}

// Helper to restore saved file descriptors
static void restore_fds(int saved[3]) {
    for (int fd = 0; fd < 3; ++fd) {
        if (saved[fd] >= 0) {
            dup2(saved[fd], fd);
            close(saved[fd]);
        }
    }
}

static int run_pipeline(pipeline_parser pp) {
    // Parse commands from the input (redirections and arguments)
    std::vector<command> commands;
    for (auto cmdp = pp.command_begin(); cmdp; ++cmdp) {
        command cmd;
        parse_redirs(cmdp, cmd.args, cmd.redirs);
        commands.push_back(cmd);
    }

    // Handle case with no commands
    if (commands.empty()) { return 0; }

    // Handle single cd command 
    if (commands.size() == 1 && !commands[0].args.empty() && commands[0].args[0] == "cd") {
        auto& cmd = commands[0];
        // Find target directory (defualt to HOME)
        const char* target = cmd.args.size() >= 2 ? cmd.args[1].c_str() : getenv("HOME");
        if (!target) { // No target directory
            perror("cd: missing operand\n");
            return make_status(1);
        }
        if (cmd.args.size() > 2) { // Not a single target directory
            perror("cd: too many arguments\n");
            return make_status(1);
        }

        // Store original file descriptors
        int saved[3] = {-1, -1, -1};
        // Apply redirections
        for (auto const& r : cmd.redirs) {
            // Open the file for redirection
            int fd = open(r.filename.c_str(), r.is_input ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC), 0666);
            if (fd < 0) {
                perror(r.filename.c_str());
                restore_fds(saved); 
                return make_status(1);
            }
            // Save original fds
            if (r.fd <= 2 && saved[r.fd] < 0) saved[r.fd] = dup(r.fd);
            if (dup2(fd, r.fd) < 0) {
                perror("dup2");
                close(fd);
                restore_fds(saved);
                return make_status(1);
            }
            close(fd);
        }
        
        // Execute cd command
        int status = chdir(target) < 0 ? 1 : 0;
        if (status) { perror("cd"); }
        restore_fds(saved);
        return make_status(status);
    }

    // Run pipeline
    std::vector<pid_t> pids;
    int prev_read_fd = -1;

    for (size_t i = 0; i < commands.size(); ++i) {
        bool last = (i == commands.size() - 1);
        int pipefd[2] = {-1, -1};

        // Create pipe to next command
        if (!last && pipe(pipefd) < 0) {
            perror("pipe");
            if (prev_read_fd >= 0) { close(prev_read_fd); }
            break;
        }

        // Run command
        commands[i].run(prev_read_fd, last ? -1 : pipefd[1]);
        if (commands[i].pid <= 0) { continue; }  // Skip empty commands
        // Track child pid 
        pids.push_back(commands[i].pid);
        // Clean up parent file descriptors
        if (prev_read_fd >= 0) {
            close(prev_read_fd);
            prev_read_fd = -1;
        }
        // Prepare for next iteration
        if (!last) {
            prev_read_fd = pipefd[0];
            close(pipefd[1]);
        }
    }

    // Ensure all pipe fds are closed before waiting
    if (prev_read_fd >= 0) {
        close(prev_read_fd);
        prev_read_fd = -1;
    }

    // Wait for all pipeline processes to finish
    int status = 0;
    for (size_t i = 0; i < pids.size(); ++i) {
        int s;
        while (waitpid(pids[i], &s, 0) == -1 && errno == EINTR);
        if (i == pids.size() - 1){ status = s; } // Return last status
    }
    return status;
}

// Evaluate conditionals
static int run_conditional(conditional_parser cp) {
    int status = 0;
    bool have_status = false;
    int prev_op = TYPE_SEQUENCE;

    for (auto pp = cp.pipeline_begin(); pp; ++pp) {
        bool should_run;

        if (!have_status) {
            // Always run first pipeline
            should_run = true;
        } else if (prev_op == TYPE_AND) {
            // Run only if previous succeeded 
            bool prev_ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);
            should_run = prev_ok;
        } else if (prev_op == TYPE_OR) {
            // Run only if previous failed
            bool prev_ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);
            should_run = !prev_ok;
        }

        if (should_run) {
            status = run_pipeline(pp);
            have_status = true;
        }

        // Check next operator
        int op = pp.next_op();
        if (op == TYPE_AND || op == TYPE_OR) {
            prev_op = op;
        } else {
            prev_op = TYPE_SEQUENCE;  // no &&/|| to affect the next one
        }
    }

    return status;
}

// run_line(clp)
//    Run the command line contained in `command_line_parser clp`.
//
//    PHASE 1: Use `waitpid` to wait for the command started by `c->run()`
//        to finish.
//
//    The remaining phases may require that you introduce helper functions
//    (e.g., to process a pipeline), write code in `command::run`, and/or
//    change `struct command`.
//
//    It is possible, and not too ugly, to handle command lines, conditionals,
//    *and* pipelines entirely within `run_line`, but in general it is clearer
//    to introduce `run_conditional` and `run_pipeline` functions that
//    are called by `run_line`. It’s up to you.
//
//    PHASE 2: Introduce a loop so `run_line` can run command lists, which
//       consist of one or more commands separated by TYPE_SEQUENCE ';'. Wait
//       for each command to finish before going on to the next.
//    PHASE 3: Change the loop to handle conditionals.
//    PHASE 4: Change the loop to handle pipelines. Start all processes in
//       the pipeline in parallel. The status of a pipeline is the status of
//       its LAST command.
//    PHASE 5: Change the loop to handle background conditional chains.
//       This may require adding another call to `fork()`!
void run_line(command_line_parser clp) {
    // Iterate over conditionals in the command line
    for (auto cp = clp.conditional_begin(); cp; ++cp) {
        // Use operator to determine if command is run in foreground or background
        int op = cp.next_op();
        bool background = (op == TYPE_BACKGROUND);

        if (!background) {
            // Foreground (run in this shell)
            run_conditional(cp);
        } else {
            // Background (fork new process to run this)
            pid_t pid = fork();
            if (pid < 0) {
                // If fork fails, run in foreground
                perror("fork");
                run_conditional(cp);
            } else if (pid == 0) {
                // Run the conditional in the child
                int status = run_conditional(cp);
                if (WIFEXITED(status)) {
                    _exit(WEXITSTATUS(status));
                } else {
                    _exit(1);
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    FILE* command_file = stdin;
    bool quiet = false;

    // Check for `-q` option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file) {
            perror(argv[1]);
            return 1;
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (!feof(command_file)) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
            } else {
                if (ferror(command_file)) {
                    perror("sh61");
                }
                break;
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            run_line(command_line_parser{buf});
            bufpos = 0;
            needprompt = 1;
        }
        // Handle zombie processes
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        }
    }

    return 0;
}
