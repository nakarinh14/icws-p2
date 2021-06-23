SRC_DIR := src
OBJ_DIR := obj
# all src files
SRC := $($(wildcard $(SRC_DIR)/*.c), $(SRC_DIR)/*.cpp) 
# all objects
OBJ := $(OBJ_DIR)/y.tab.o $(OBJ_DIR)/lex.yy.o $(OBJ_DIR)/parse.o ${OBJ_DIR}/pcsa_net.o ${OBJ_DIR}/icws.o ${OBJ_DIR}/cgi_helper.o 
# all binaries
CXXOBJ :=  ${OBJ_DIR}/icws.o  ${OBJ_DIR}/cgi_helper.o 
# CPP binaryes
BIN := icws
# C compiler
CC  := gcc 
# C PreProcessor Flag
CPPFLAGS := 
# compiler flags
CFLAGS   := -g -Wall -std=gnu17
CXXFLAGS := -std=gnu++17
# DEPS = parse.h y.tab.h
LIBFLAGS=-pthread

default: all
all : icws

icws: $(OBJ)
	g++ $^ $(LIBFLAGS) -o $@

$(SRC_DIR)/lex.yy.c: $(SRC_DIR)/lexer.l
	flex -o $@ $^

$(SRC_DIR)/y.tab.c: $(SRC_DIR)/parser.y
	yacc -Wno-yacc -d $^
	mv y.tab.c $@
	mv y.tab.h $(SRC_DIR)/y.tab.h

$(OBJ_DIR)/icws.o: $(SRC_DIR)/icws.cpp $(OBJ_DIR)
	g++ -g -Wall $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cgi_helper.o: $(SRC_DIR)/cgi_helper.cpp $(OBJ_DIR)
	g++ -g -Wall $(CXXFLAGS) -c $< -o $@


$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

#echo_client: $(OBJ_DIR)/echo_client.o
#	$(CC) -Werror $^ -o $@

$(OBJ_DIR):
	mkdir $@

clean:
	$(RM) $(OBJ) $(BIN) $(SRC_DIR)/lex.yy.c $(SRC_DIR)/y.tab.*
	$(RM) -r $(OBJ_DIR)
