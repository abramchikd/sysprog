#include "process.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <assert.h>
#include <string.h>

struct process
{
	pid_t pid;
	int out_pipe[2];
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
execute_part(const struct command_line *line, struct expr **e_start);

static int
create_out_descriptor(const struct command_line *line);

static bool
is_end_expression(const struct expr *e);

static void
exec_cmd(const struct command *cmd, struct process_collection *collection);

static int
run_fork(const struct command *cmd, int in, int out_pipe[]);

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
execute_command_line(const struct command_line *line)
{
	bool is_forked = false;
	if (line->is_background) {
		if (fork() != 0) {
			return 0;
		}

		is_forked = true;
	}

	struct expr *e_start = line->head;
	int res;
	for (;;) {
		res = execute_part(line, &e_start);
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
		exit(res);
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
execute_part(const struct command_line *line, struct expr **e_start)
{
	struct process_collection collection = {NULL, 0, 0};

	bool has_exit = false;
	int exit_code = 0;
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

		if (strcmp(e->cmd.exe, "exit") == 0) {
			exit_code = e->cmd.arg_count == 1 ? 0 : atoi(e->cmd.args[1]);
			if (collection.size == 0 && is_end_expression(e->next)) {
				exit(exit_code);
			}

			has_exit = collection.size != 0;
			e = e->next;
			continue;
		}

		exec_cmd(&e->cmd, &collection);
		exit_code = 0;
		e = e->next;
	}

	if (collection.size != 0) {
		int file = create_out_descriptor(line);
		int in = collection.processes[collection.size -1].out_pipe[STDIN_FILENO];
		char buffer[255];
		ssize_t size;
		while ((size = read(in, buffer, 255)) != 0) {
			if (!has_exit) {
				write(file, buffer, size);
			}
		}

		if (file != STDOUT_FILENO) {
			close(file);
		}
	}

	if (collection.size != 0) {
		close(collection.processes[collection.size -1].out_pipe[STDIN_FILENO]);
	}

	for (int i = 0; i < collection.size; i++) {
		int status;
		waitpid(collection.processes[i].pid, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			exit_code = WEXITSTATUS(status);
		}
	}

	*e_start = e;
	return exit_code;
}

static bool
is_end_expression(const struct expr *e)
{
	return e == NULL || e->type == EXPR_TYPE_AND || e->type == EXPR_TYPE_OR;
}

static void
exec_cmd(const struct command *cmd, struct process_collection *collection)
{
	struct process proc;
	pipe(proc.out_pipe);

	proc.pid = run_fork(
		cmd,
		collection->size == 0 ? STDIN_FILENO : collection->processes[collection->size - 1].out_pipe[STDIN_FILENO],
		proc.out_pipe
	);

	close(proc.out_pipe[STDOUT_FILENO]);

	if (collection->size != 0) {
		close(collection->processes[collection->size - 1].out_pipe[STDIN_FILENO]);
	}

	process_collection_append(collection, &proc);
}

static int
run_fork(const struct command *cmd, int in, int out_pipe[])
{
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}

	if (in != STDIN_FILENO) {
		dup2(in, STDIN_FILENO);
		close(in);
	}

	dup2(out_pipe[STDOUT_FILENO], STDOUT_FILENO);
	close(out_pipe[STDOUT_FILENO]);
	close(out_pipe[STDIN_FILENO]);

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