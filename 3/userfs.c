#include "userfs.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum
{
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block
{
	/** Block memory. */
	char memory[BLOCK_SIZE];
	/** How many bytes are occupied. */
	size_t occupied;
	/** Next block in the file. */
	struct block *next;
	/** Previous block in the file. */
	struct block *prev;

	/* PUT HERE OTHER MEMBERS */
};

struct file
{
	/** Double-linked list of file blocks. */
	struct block *block_list;
	/**
	 * Last block in the list above for fast access to the end
	 * of file.
	 */
	struct block *last_block;
	/** How many file descriptors are opened on the file. */
	int refs;
	/** File name. */
	char *name;
	/** Files are stored in a double-linked list. */
	struct file *next;
	struct file *prev;
	size_t size;
	size_t descriptors_count;

	/* PUT HERE OTHER MEMBERS */
};

struct filedesc
{
	struct file *file;
	size_t position;
	size_t block_number;
	struct block *current_block;
	int flags;
};

/** List of all files. */
static struct file *file_list = NULL;

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

static struct file *
create_file(const char *filename);

static int
create_descriptor(struct file *file, int flags);

int
ufs_open(const char *filename, int flags)
{
	if (file_list != NULL) {
		if (strcmp(file_list->name, filename) == 0) {
			return create_descriptor(file_list, flags);
		}

		struct file *current = file_list->next;
		while (current != file_list) {
			if (strcmp(current->name, filename) == 0) {
				return create_descriptor(current, flags);
			}

			current = current->next;
		}
	}

	if (!(flags & UFS_CREATE)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	return create_descriptor(create_file(filename), flags);
}

static struct file *
create_file(const char *filename)
{
	struct file *file = calloc(1, sizeof(struct file));
	size_t name_length = strlen(filename) + 1;
	file->name = malloc(sizeof(char) * name_length);
	file->descriptors_count = 1;
	memcpy(file->name, filename, name_length);

	if (file_list == NULL) {
		file_list = file;
		file->next = file;
		file->prev = file;
		return file;
	}

	file->prev = file_list->prev;
	file->next = file_list;
	file_list->prev = file;
	file->prev->next = file;

	return file;
}

static int
create_descriptor(struct file *file, int flags)
{
	struct filedesc *descriptor = calloc(1, sizeof(struct filedesc));
	descriptor->file = file;
	descriptor->current_block = file->block_list;
	descriptor->flags = flags;
	file->descriptors_count++;

	if (file_descriptors == NULL) {
		file_descriptors = calloc(1, sizeof(*file_descriptors));
		file_descriptor_capacity = 1;
	}

	for (int i = 0; i < file_descriptor_count; i++) {
		if (file_descriptors[i] != NULL) {
			continue;
		}

		file_descriptors[i] = descriptor;
		return i;
	}

	if (file_descriptor_count >= file_descriptor_capacity) {
		file_descriptor_capacity = (file_descriptor_capacity + 1) * 2;
		file_descriptors = realloc(file_descriptors, sizeof(*file_descriptors) * file_descriptor_capacity);
	}

	file_descriptors[file_descriptor_count++] = descriptor;
	return file_descriptor_count - 1;
}

static void
hop_to_next_block(struct filedesc *desc);

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	if (fd > file_descriptor_count || fd < 0 || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (size == 0) {
		return 0;
	}

	struct filedesc *desc = file_descriptors[fd];
	if (desc->flags & UFS_READ_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (desc->current_block == NULL) {
		desc->file->block_list = calloc(1, sizeof(struct block));
		desc->file->last_block = desc->file->block_list;
		desc->current_block = desc->file->block_list;
	}

	if (desc->block_number * BLOCK_SIZE + desc->position > desc->file->size) {
		desc->block_number = desc->file->size / BLOCK_SIZE;
		desc->position = desc->file->size % BLOCK_SIZE;
		desc->current_block = desc->file->block_list;
		for (size_t i = 0; i < desc->block_number; i++) {
			desc->current_block = desc->current_block->next;
		}
	}

	size_t size_left = size;
	while (size_left > 0) {
		size_t size_to_write = BLOCK_SIZE - desc->position;
		if (size_to_write == 0) {
			hop_to_next_block(desc);
			continue;
		}

		if (size_to_write > size_left) {
			size_to_write = size_left;
		}

		memcpy(desc->current_block->memory + desc->position, buf, size_to_write);
		desc->position += size_to_write;

		if (desc->current_block->occupied < desc->position) {
			desc->file->size += desc->position - desc->current_block->occupied;
			desc->current_block->occupied = desc->position;
			if (desc->file->size > MAX_FILE_SIZE) {
				ufs_error_code = UFS_ERR_NO_MEM;
				return -1;
			}
		}

		buf += size_to_write;
		size_left -= size_to_write;
	}

	return (ssize_t) size;
}

static void
hop_to_next_block(struct filedesc *desc)
{
	if (desc->current_block->next == NULL) {
		struct block *new_block = calloc(1, sizeof(struct block));
		new_block->prev = desc->current_block;
		desc->current_block->next = new_block;
		desc->file->last_block = new_block;
	}

	desc->current_block = desc->current_block->next;
	desc->position = 0;
	desc->block_number++;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	if (fd > file_descriptor_count || fd < 0 || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (size == 0) {
		return 0;
	}

	struct filedesc *desc = file_descriptors[fd];
	if (desc->flags & UFS_WRITE_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (desc->current_block == NULL) {
		desc->current_block = desc->file->block_list;
		if (desc->current_block == NULL) {
			return 0;
		}
	}

	if (desc->block_number * BLOCK_SIZE + desc->position > desc->file->size) {
		desc->block_number = desc->file->size / BLOCK_SIZE;
		desc->position = desc->file->size % BLOCK_SIZE;
		desc->current_block = desc->file->block_list;
		for (size_t i = 0; i < desc->block_number; i++) {
			desc->current_block = desc->current_block->next;
		}
	}

	if (desc->current_block->occupied == desc->position && desc->current_block->next == NULL) {
		return 0;
	}

	size_t read = 0;
	while (read < size) {
		if (desc->current_block->occupied - desc->position >= size - read) {
			memcpy(buf, desc->current_block->memory + desc->position, size - read);
			desc->position += size - read;
			return size;
		}

		if (desc->current_block->occupied != BLOCK_SIZE || desc->current_block->next == NULL) {
			memcpy(buf, desc->current_block->memory + desc->position, desc->current_block->occupied - desc->position);
			read += desc->current_block->occupied - desc->position;
			desc->position = desc->current_block->occupied;
			return read;
		}

		memcpy(buf, desc->current_block->memory + desc->position, BLOCK_SIZE - desc->position);
		read += BLOCK_SIZE - desc->position;
		buf += BLOCK_SIZE - desc->position;
		desc->position = 0;
		desc->current_block = desc->current_block->next;
		desc->block_number++;
	}

	return read;
}

static void
free_file(struct file *file);

int
ufs_close(int fd)
{
	if (fd > file_descriptor_count || fd < 0 || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct filedesc *desc = file_descriptors[fd];
	desc->file->descriptors_count--;
	if (desc->file->descriptors_count == 0) {
		free_file(desc->file);
	}

	free(desc);
	file_descriptors[fd] = NULL;

	return 0;
}

int
ufs_delete(const char *filename)
{
	if (file_list == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct file *file = NULL;
	if (strcmp(file_list->name, filename) == 0) {
		file = file_list;
	} else {
		struct file *current = file_list->next;
		while (current != file_list) {
			if (strcmp(current->name, filename) == 0) {
				file = current;
				break;
			}

			current = current->next;
		}
	}

	if (file == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (file->prev == file) {
		file_list = NULL;
	} else {
		file->prev->next = file->next;
		file->next->prev = file->prev;
	}

	file->descriptors_count--;
	if (file->descriptors_count == 0) {
		free_file(file);
	}

	return 0;
}

static void
free_all_blocks(struct file *file);

static void
free_file(struct file *file)
{
	free(file->name);
	free_all_blocks(file);
	free(file);
}

#if NEED_RESIZE

static void
expand(struct file *file, size_t new_size);

static void
shrink(struct file *file, size_t new_size);

int
ufs_resize(int fd, size_t new_size)
{
	if (fd > file_descriptor_count || fd < 0 || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct filedesc *desc = file_descriptors[fd];
	if (desc->flags & UFS_READ_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (new_size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	if (desc->file->size < new_size) {
		expand(desc->file, new_size);
	}

	if (desc->file->size > new_size) {
		shrink(desc->file, new_size);
	}

	return 0;
}

static void
expand(struct file *file, size_t new_size)
{
	if (new_size == 0) {
		return;
	}

	size_t normalized_new_size = new_size % BLOCK_SIZE == 0
		                             ? new_size
		                             : new_size + (BLOCK_SIZE - new_size % BLOCK_SIZE);

	size_t normalized_file_size = file->size % BLOCK_SIZE == 0
		                              ? file->size
		                              : file->size + (BLOCK_SIZE - file->size % BLOCK_SIZE);

	if (file->block_list == NULL) {
		struct block *new_block = calloc(1, sizeof(struct block));
		file->last_block = new_block;
		file->block_list = new_block;
		normalized_file_size += BLOCK_SIZE;
	}

	while (normalized_file_size < normalized_new_size) {
		struct block *new_block = calloc(1, sizeof(struct block));
		new_block->prev = file->last_block;
		file->last_block->next = new_block;
		file->last_block = new_block;
		normalized_file_size += BLOCK_SIZE;
	}
}

static void
shrink(struct file *file, size_t new_size)
{
	if (new_size == 0) {
		free_all_blocks(file);
		return;
	}

	while (file->size > new_size) {
		if (file->size == BLOCK_SIZE || file->size / BLOCK_SIZE == new_size / BLOCK_SIZE) {
			file->size = new_size;
			file->last_block->occupied = new_size % BLOCK_SIZE;
			return;
		}

		file->size -= file->last_block->occupied;
		struct block *tmp = file->last_block;
		file->last_block->prev->next = NULL;
		file->last_block = file->last_block->prev;
		free(tmp);
	}
}

#endif

static void
free_all_blocks(struct file *file)
{
	struct block *block = file->block_list;
	while (block != NULL) {
		struct block *cur = block;
		block = block->next;
		free(cur);
	}

	file->block_list = NULL;
	file->last_block = NULL;
	file->size = 0;
}

void
ufs_destroy(void)
{
	for (int i = 0; i < file_descriptor_count; i++) {
		if (file_descriptors[i] != NULL) {
			ufs_close(i);
		}
	}

	free(file_descriptors);
	if (file_list == NULL) {
		return;
	}

	struct file *file = file_list->next;
	while (file->next != file_list) {
		ufs_delete(file->name);
	}

	ufs_delete(file_list->name);
}
