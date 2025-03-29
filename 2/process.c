#include "process.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <assert.h>
#include <string.h>

struct process
{
	pid_t pid;
	int out_pipe[2];
	int exit_code;
};

struct process_collection
{
	struct process *processes;
	int size;
	int capacity;
};

static void
process_collection_append(struct process_collection *collection, const struct process *process);

static void
skip_or(struct expr **e);

static int
execute_part(const struct command_line *line, struct expr **e_start, bool *need_exit);

static int
create_out_descriptor(const struct command_line *line);

static bool
is_end_expression(const struct expr *e);

static void
exec_cmd(const struct command *cmd, struct process_collection *collection, int last_out);

static int
run_fork(const struct command *cmd, int in, int out_pipe[], int last_out);

static void
process_collection_append(struct process_collection *collection, const struct process *process)
{
	if (collection->size == collection->capacity) {
		collection->capacity = (collection->capacity + 1) * 2;
		collection->processes = realloc(collection->processes, sizeof(*collection->processes) * collection->capacity);
	} else {
		assert(collection->size < collection->capacity);
	}

	collection->processes[collection->size++] = *process;
}

int
execute_command_line(const struct command_line *line, bool *need_exit)
{
	bool is_forked = false;
	if (line->is_background) {
		int pid = fork();
		if (pid != 0) {
			int status;
			waitpid(pid, &status, 0);
			return 0;
		}

		if (fork() != 0) {
			*need_exit = true;
			return 0;
		}

		is_forked = true;
	}

	struct expr *e_start = line->head;
	int res;
	for (;;) {
		res = execute_part(line, &e_start, need_exit);
		if (*need_exit) {
			return res;
		}

		if (e_start == NULL) {
			break;
		}

		if (e_start->type == EXPR_TYPE_AND) {
			if (res == 0) {
				e_start = e_start->next;
				continue;
			}

			break;
		}

		if (e_start->type == EXPR_TYPE_OR) {
			if (res != 0) {
				e_start = e_start->next;
				continue;
			}

			skip_or(&e_start);
			if (e_start == NULL) {
				break;
			}

			continue;
		}

		assert(false);
	}

	if (is_forked) {
		*need_exit = true;
	}

	return res;
}

static void
skip_or(struct expr **e)
{
	while (*e != NULL) {
		if ((*e)->type == EXPR_TYPE_AND) {
			return;
		}

		*e = (*e)->next;
	}
}

static int
execute_part(const struct command_line *line, struct expr **e_start, bool *need_exit)
{
	struct process_collection collection = {NULL, 0, 0};

	int out_file = -1;
	struct expr *e = *e_start;
	while (e != NULL) {
		if (e->type == EXPR_TYPE_PIPE) {
			e = e->next;
			continue;
		}

		if (is_end_expression(e)) {
			break;
		}

		if (strcmp(e->cmd.exe, "cd") == 0) {
			chdir(e->cmd.args[1]);
			e = e->next;
			continue;
		}

		if (strcmp(e->cmd.exe, "exit") == 0 && collection.size == 0 && is_end_expression(e->next)) {
			free(collection.processes);
			*need_exit = true;
			return e->cmd.arg_count == 1 ? 0 : atoi(e->cmd.args[1]);
		}

		if (is_end_expression(e->next)) {
			out_file = e->next == NULL ? create_out_descriptor(line) : STDOUT_FILENO;
		}

		exec_cmd(
			&e->cmd,
			&collection,
			out_file
		);

		e = e->next;
	}

	int exit_code = 0;
	for (int i = 0; i < collection.size; i++) {
		if (collection.processes[i].pid == -1) {
			exit_code = collection.processes[i].exit_code;
			continue;
		}

		int status;
		waitpid(collection.processes[i].pid, &status, 0);
		exit_code = WEXITSTATUS(status);
	}

	if (out_file != -1 && out_file != STDOUT_FILENO) {
		close(out_file);
	}

	free(collection.processes);

	*e_start = e;
	return exit_code;
}

static bool
is_end_expression(const struct expr *e)
{
	return e == NULL || e->type == EXPR_TYPE_AND || e->type == EXPR_TYPE_OR;
}

static void
exec_cmd(const struct command *cmd, struct process_collection *collection, int last_out)
{
	struct process proc;

	if (last_out == -1) {
		pipe(proc.out_pipe);
	} else {
		proc.out_pipe[STDIN_FILENO] = -1;
		proc.out_pipe[STDOUT_FILENO] = -1;
	}

	if (strcmp(cmd->exe, "exit") != 0) {
		proc.pid = run_fork(
			cmd,
			collection->size == 0 ? STDIN_FILENO : collection->processes[collection->size - 1].out_pipe[STDIN_FILENO],
			proc.out_pipe,
			last_out
		);
	} else {
		proc.pid = -1;
		proc.exit_code = cmd->arg_count == 1 ? 0 : atoi(cmd->args[1]);
	}

	if (proc.out_pipe[STDOUT_FILENO] != -1) {
		close(proc.out_pipe[STDOUT_FILENO]);
	}

	if (collection->size != 0 && collection->processes[collection->size - 1].out_pipe[STDIN_FILENO] != -1) {
		close(collection->processes[collection->size - 1].out_pipe[STDIN_FILENO]);
	}

	process_collection_append(collection, &proc);
}

static int
run_fork(const struct command *cmd, int in, int out_pipe[], int last_out)
{
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}

	if (in != STDIN_FILENO) {
		dup2(in, STDIN_FILENO);
		close(in);
	}

	if (last_out == -1) {
		dup2(out_pipe[STDOUT_FILENO], STDOUT_FILENO);
		close(out_pipe[STDOUT_FILENO]);
		close(out_pipe[STDIN_FILENO]);
	} else {
		if (last_out != STDOUT_FILENO) {
			dup2(last_out, STDOUT_FILENO);
			close(last_out);
		}
	}

	execvp(cmd->exe, cmd->args);
	return 0;
}

static int
create_out_descriptor(const struct command_line *line)
{
	int file = STDOUT_FILENO;
	if (line->out_type == OUTPUT_TYPE_STDOUT) {
		return file;
	}

	if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
		file = open(line->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0000644);
		if (file == -1) {
			write(1, "Error", 6);
		}

		return file;
	}

	if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
		file = open(line->out_file, O_WRONLY | O_CREAT | O_APPEND, 0000644);
		if (file == -1) {
			write(1, "Error", 6);
		}

		return file;
	}

	assert(false);
}
