# important directories
SRC = src
BIN = bin
OBJ = obj
INC = include

# compilation related parameters
CC      = gcc
CFLAGS  = -ggdb
LDFLAGS = -lpthread

# identify sources; generate objects and final binary targets
SOURCES = $(wildcard $(SRC)/*.c)
OBJECTS = $(patsubst $(SRC)/%.c, $(OBJ)/%.o, $(SOURCES))
TARGETS = $(patsubst $(SRC)/%.c, $(BIN)/%,   $(SOURCES))

# directive to prevent (attempted) intermediary file/directory deletion
.PRECIOUS: $(BIN)/ $(OBJ)/

# top level rule
build: $(TARGETS)

# generate compile_commands.json for clangd (language server)
bear:
	bear -- $(MAKE) build

# non-persistent direcotry creation rule
%/:
	@mkdir -p $@

# final binary generation rule
$(BIN)/%: $(OBJ)/%.o | $(BIN)/
	$(CC) -o $@ $^ $(LDFLAGS)

# object generation rule
$(OBJ)/%.o: $(SRC)/%.c | $(OBJ)/
	$(CC) -c -I $(INC) $(CFLAGS) -o $@ $^

# clean rule
clean:
	@rm -rf $(BIN) $(OBJ)

