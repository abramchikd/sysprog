GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -g

all: lib exe test

lib: chat.c chat_client.c chat_server.c
	gcc $(GCC_FLAGS) -c chat.c -o chat.o
	gcc $(GCC_FLAGS) -c chat_client.c -o chat_client.o
	gcc $(GCC_FLAGS) -c chat_server.c -o chat_server.o

exe: lib chat_client_exe.c chat_server_exe.c
	gcc $(GCC_FLAGS) chat_client_exe.c chat.o chat_client.o -o client
	gcc $(GCC_FLAGS) chat_server_exe.c chat.o chat_server.o -o server

test: lib
	gcc $(GCC_FLAGS) test.c chat.o chat_client.o chat_server.o -o test 	\
		../utils/unit.c -I ../utils -lpthread

heap:
	gcc $(GCC_FLAGS) -ldl -rdynamic test.c chat.c chat_client.c chat_server.c ../utils/heap_help/heap_help.c -o test \
		../utils/unit.c -I ../utils -lpthread -I ../utils/heap_help

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -lpthread -o test

clean:
	rm *.o
	rm client server test
