#include "sh61.hh"
#include <cctype>
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>

// For the love of God
#undef exit
#define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__

// Stores the last foreground command's exit status for $ expansion
static int last_exit_status = 0;
// Flag for SIGINT handling
static volatile sig_atomic_t sigint_received = 0;
// Signal handler for SIGINT (sets flag)
static void sigint_handler(int) {
    sigint_received = 1;
}
// Flag to track if program is running in a background child process
static bool is_background_child = false;

/// expand_variables(s)
///     Expands shell variables in a string s.
///     Supports ${VAR}, $VAR, $?, $$
static std::string expand_variables(const std::string& s) {
    std::string result;
    size_t i = 0;
    // Iterate through string characters
    while (i < s.size()) {
        if (s[i] == '$' && i + 1 < s.size()) { // If variable reference found
            ++i;
            std::string varname;

            if (s[i] == '{') { // Check for ${VAR} syntax
                ++i;
                // Extract variable name
                while (i < s.size() && s[i] != '}') {
                    varname += s[i++];
                }
                // Check for and skip closing brace
                if (i < s.size() && s[i] == '}') {
                    ++i; 
                }
            }
            // Check for special single-characters
            else if (s[i] == '?') {
                // $? returns last exit status
                result += std::to_string(last_exit_status);
                ++i;
                continue;
            }
            else if (s[i] == '$') {
                // $$ returns shell PID
                result += std::to_string(getpid());
                ++i;
                continue;
            }
            else { // Check for regular $VAR
                while (i < s.size() && (isalnum(s[i]) || s[i] == '_')) { // only alphanumeric and _ valid
                    varname += s[i++];
                }
            }
            // Find variable value 
            if (!varname.empty()) {
                const char* value = getenv(varname.c_str());
                if (value) { // If variable has value
                    result += value;
                }
            }
        } else { // If not variable reference, copy character
            result += s[i++];
        }
    }
    
    return result;
}

struct redirection {
    int fd;               
    std::string filename;
    bool is_input;        // true for < redirections
    bool append;          // true for >> redirections
};

struct command {
    std::vector<std::string> args;
    std::vector<redirection> redirs;
    std::string subshell_content;  // If non-empty, this is a subshell
    pid_t pid = -1;

    command();
    ~command();

    void run(int stdin_fd = -1, int stdout_fd = -1, pid_t pgid = 0);
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

// Helper command to apply redirections before running a command
static void apply_redirections(const std::vector<redirection>& redirs) {
    for (auto const& r : redirs) {
        // Determine open flags based on redirection type
        int flags;
        if (r.is_input) { // If input redirection
            flags = O_RDONLY;
        } else if (r.append) { // If append redirection
            flags = O_WRONLY | O_CREAT | O_APPEND;
        } else { // If output redirection
            flags = O_WRONLY | O_CREAT | O_TRUNC;
        }
        int fd = open(r.filename.c_str(), flags, 0666);
        
        // Handle error from making file
        if (fd < 0) {
            perror(r.filename.c_str());
            _exit(1);
        }
        
        // Point file descriptor to the newly opened file
        if (dup2(fd, r.fd) < 0) { // Handle error 
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

void command::run(int stdin_fd, int stdout_fd, pid_t pgid) {
    assert(this->pid == -1);
    
    // Handle empty command (no args, no subshell, no redirections)
    if (this->args.empty() && this->subshell_content.empty() && this->redirs.empty()) {
        this->pid = 0; 
        return;
    }

    // Build argv for regular commands
    std::vector<char*> argv;
    if (!this->subshell_content.empty()) {
        // Subshells (run_line) don't require argv
    } else {
        // Push regular command args to argv
        argv.reserve(this->args.size() + 1);
        for (auto& s : this->args) {
            argv.push_back(const_cast<char*>(s.c_str()));
        }
        argv.push_back(nullptr);
    }

    pid_t p = fork();

    if (p < 0) { // Handle fork error
        perror("fork");
        _exit(EXIT_FAILURE);
    } else if (p == 0) {
        // Child process

        // Set up process group for interrupt handling
        // If group already exists, will join the existing group
        setpgid(0, pgid);
        
        // Set up pipeline connections
        if (stdin_fd >= 0) { // copy old stdin to new stdin
            if (dup2(stdin_fd, STDIN_FILENO) < 0) {
                perror("dup2");
                _exit(1);
            }
            close(stdin_fd);
        }
        if (stdout_fd >= 0) { // copy old stdout to new stdout
            if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
                perror("dup2");
                _exit(1);
            }
            close(stdout_fd);
        }
        // Ensure all other pipe file descriptors are closed
        for (int fd = 3; fd < 1024; ++fd) {
            close(fd);  
        }

        // Apply redirections
        apply_redirections(this->redirs);

        if (!this->subshell_content.empty()) { // If this is a subshell
            // Execute the subshell content as a command line
            run_line(command_line_parser{this->subshell_content.c_str()});
            _exit(last_exit_status);
        }
        
        // Otherwise, run the regular command
        if (argv[0] != nullptr) {
            execvp(argv[0], argv.data());
            perror(argv[0]);
            _exit(EXIT_FAILURE);
        } else { // Handle case with no command
            _exit(0);
        }
    }
    
    // Parent process

    // Set child's process group from parent
    // Avoids race condition (parent might run before child after fork)
    setpgid(p, pgid == 0 ? p : pgid);
    
    // Save child pid
    this->pid = p;
}

// Helper function to parse a command (supports commands, subshells, and redirs) from command line input
static void parse_command(command_parser cmdp, command& cmd) {
    cmd.args.clear();
    cmd.redirs.clear();
    cmd.subshell_content.clear();

    // Track state for redirection parsing
    bool expect_filename = false;
    int pending_fd = -1;
    bool pending_is_input = false;
    bool pending_append = false;
    
    // Prepare to iterate through tokens
    auto tok = cmdp.token_begin();
    
    if (tok != cmdp.end() && tok.type() == TYPE_LPAREN) { // If this is a subshell
        ++tok;  // Skip the (
        
        // Track subshell depth
        int paren_depth = 1;
        
        // Iterate through tokens, collecting subshell content
        std::string subshell_str;
        while (tok != cmdp.end() && paren_depth > 0) {
            if (tok.type() == TYPE_LPAREN) { // Handle ( 
                paren_depth++;
                subshell_str += "( ";
            } else if (tok.type() == TYPE_RPAREN) { // Handle )
                paren_depth--;
                if (paren_depth > 0) {
                    subshell_str += ") ";
                }
            } else { // Add normal tokens to subshell content
                subshell_str += tok.str() + " ";
            }
            ++tok;
        }
        
        // Save subshell content
        cmd.subshell_content = subshell_str;
    }
    
    // Parse remaining tokens (redirections after subshell or commands)
    for (; tok != cmdp.end(); ++tok) { // Continue iterating through tokens
        int t = tok.type();

        // Handle different token types
        if (expect_filename) { // If token is a filename following a redirect operator
            if (t == TYPE_NORMAL) {
                // Create redirection object and add to list of redirections
                redirection r;
                r.fd = pending_fd;
                r.filename = expand_variables(tok.str());
                r.is_input = pending_is_input;
                r.append = pending_append;
                cmd.redirs.push_back(r);
                expect_filename = false;
                continue;
            }
        } 
        if (t == TYPE_REDIRECT_OP) { // If token is a redirector
            // Get redirect operator string
            std::string op = tok.str();
            
            // Parse leading file descriptor number (N in N>)
            size_t i = 0;
            int fd = -1;
            while (i < op.size() && isdigit(op[i])) {
                if (fd < 0) fd = 0;
                fd = fd * 10 + (op[i] - '0');
                ++i;
            }
            
            // Parse the operator itself
            bool is_input = false;
            bool append = false;
            if (i < op.size() && op[i] == '<') { // If operator is <
                is_input = true;
                if (fd < 0) fd = STDIN_FILENO;  // default fd for <
            } else if (i < op.size() && op[i] == '>') { // If operator is >
                is_input = false;
                if (fd < 0) fd = STDOUT_FILENO;  // default fd for >
                // Check for >> (append)
                if (i + 1 < op.size() && op[i + 1] == '>') {
                    append = true;
                }
            }
            
            pending_fd = fd;
            pending_is_input = is_input;
            pending_append = append;
            expect_filename = true;
        } else if (t == TYPE_NORMAL && cmd.subshell_content.empty()) {
            // Regular command argument added to args vector
            cmd.args.push_back(expand_variables(tok.str()));
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
    // Parse commands from the input (redirections, arguments, and subshells)
    std::vector<command> commands;
    // Iterate through commands, parsing each
    for (auto cmdp = pp.command_begin(); cmdp; ++cmdp) {
        command cmd;
        parse_command(cmdp, cmd);
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
            // Determine open flags based on redirection type
            int flags;
            if (r.is_input) { // If input redirection
                flags = O_RDONLY;
            } else if (r.append) { // If append redirection
                flags = O_WRONLY | O_CREAT | O_APPEND;
            } else { // If output redirection
                flags = O_WRONLY | O_CREAT | O_TRUNC;
            }
            int fd = open(r.filename.c_str(), flags, 0666);
            
            // Handle error from making file
            if (fd < 0) {
                perror(r.filename.c_str());
                restore_fds(saved); 
                return make_status(1);
            }
            // Save original fds
            if (r.fd <= 2 && saved[r.fd] < 0) saved[r.fd] = dup(r.fd);
            if (dup2(fd, r.fd) < 0) { // Handle error
                perror("dup2");
                close(fd);
                restore_fds(saved);
                return make_status(1);
            }
            // Close original file
            close(fd);
        }
        
        // Execute cd command
        int status = chdir(target) < 0 ? 1 : 0;
        if (status) { perror("cd"); } // Handle error
        restore_fds(saved); // Restore original file descriptors
        return make_status(status);
    }

    // Run pipeline
    std::vector<pid_t> pids;
    int prev_read_fd = -1;
    pid_t pgid = 0;  // Process group ID for this pipeline

    for (size_t i = 0; i < commands.size(); ++i) {
        bool last = (i == commands.size() - 1);
        int pipefd[2] = {-1, -1};

        // Create pipe to next command
        if (!last && pipe(pipefd) < 0) {
            perror("pipe");
            if (prev_read_fd >= 0) { close(prev_read_fd); }
            break;
        }

        // Run command with process group
        // First command uses pgid=0 to create new group, rest join that group
        commands[i].run(prev_read_fd, last ? -1 : pipefd[1], pgid);
        if (commands[i].pid <= 0) { continue; }  // Skip empty commands
        
        // First valid process defines the process group for the pipeline
        if (pgid == 0) {
            pgid = commands[i].pid;
        }
        
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

    // Set this pipeline as the foreground process group
    // Control-C will send SIGINT to this pipeline, but only when it is the foreground process group
    if (pgid > 0 && !is_background_child) {
        claim_foreground(pgid);
    }

    // Wait for all pipeline processes to finish
    int status = 0;
    for (size_t i = 0; i < pids.size(); ++i) {
        int s;
        while (waitpid(pids[i], &s, 0) == -1 && errno == EINTR);
        if (i == pids.size() - 1){ status = s; } // Return last status
    }
    
    // Return foreground to the shell if we claimed it
    if (!is_background_child) {
        claim_foreground(0);
    }
    
    return status;
}

// Evaluate conditionals
static int run_conditional(conditional_parser cp) {
    int status = 0;
    bool have_status = false;
    int prev_op = TYPE_SEQUENCE;

    for (auto pp = cp.pipeline_begin(); pp; ++pp) {
        // If SIGINT received, cancel the rest of the conditional
        if (sigint_received) {
            break;
        }
        
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
            
            // If child was killed by signal, treat as failure and check if we should stop
            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
                sigint_received = 1; // Set flag
                break; // Cancel rest of conditional
            }          
            // Also check if shell received SIGINT
            if (sigint_received) {
                break;
            }
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
            int status = run_conditional(cp);
            // Update last_exit_status for $? variable expansion
            if (WIFEXITED(status)) {
                last_exit_status = WEXITSTATUS(status);
            } else {
                last_exit_status = 1; // Handle abnormal exit
            }
        } else {
            // Background (fork new process to run this)
            pid_t pid = fork();
            if (pid < 0) {
                // If fork fails, run in foreground
                perror("fork");
                run_conditional(cp);
            } else if (pid == 0) {
                // Mark that we're in a background child process
                is_background_child = true;
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
    
    // Setup signal handler for SIGINT
    set_signal_handler(SIGINT, sigint_handler);

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
            // Reset interrupt flag before running new command line
            sigint_received = 0;
            run_line(command_line_parser{buf});
            bufpos = 0;
            needprompt = 1;
        }
        
        // Handle SIGINT (reset buffer and show new prompt)
        if (sigint_received) {
            sigint_received = 0;
            bufpos = 0;
            buf[0] = '\0';
            needprompt = true;
            if (!quiet) {
                printf("\n");
            }
        }
        // Handle zombie processes
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        }
    }

    return 0;
}
