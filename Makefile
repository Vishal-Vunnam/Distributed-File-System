all: dfc dfs


OPENSSL_PATH = /opt/homebrew/opt/openssl@3

dfc: dfc.c 
	gcc -Wall -Wextra -o dfc dfc.c -I$(OPENSSL_PATH)/include -L$(OPENSSL_PATH)/lib -lssl -lcrypto

dfs: dfs.c
	gcc -Wall -Wextra -o dfs dfs.c

clean:
	rm -f dfc dfs *.o