GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -g

all:
	gcc $(GCC_FLAGS) solution.c parser.c process.c -o mybash

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c -o mybash
