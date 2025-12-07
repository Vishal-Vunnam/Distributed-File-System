all: dfc_cpp dfs_cpp


OPENSSL_PATH = /opt/homebrew/opt/openssl@3

# C versions
dfc: dfc.c
	gcc -Wall -Wextra -o dfc dfc.c -I$(OPENSSL_PATH)/include -L$(OPENSSL_PATH)/lib -lssl -lcrypto

dfs: dfs.c
	gcc -Wall -Wextra -o dfs dfs.c

# C++ versions
dfc_cpp: dfc.cpp
	g++ -Wall -Wextra -std=c++11 -o dfc dfc.cpp -I$(OPENSSL_PATH)/include -L$(OPENSSL_PATH)/lib -lssl -lcrypto

dfs_cpp: dfs.cpp
	g++ -Wall -Wextra -std=c++11 -o dfs dfs.cpp

clean:
	rm -rf dfc dfs *.o 


