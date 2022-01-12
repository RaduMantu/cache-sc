# important directories
SRC = src
BIN = bin
OBJ = obj
INC = include

# compilation related parameters
CXX     = g++
CFLAGS  = -ggdb -std=c++20
LDFLAGS = -lpthread -ldl

# identify sources; generate objects and final binary targets
SOURCES = $(wildcard $(SRC)/*.cpp)
OBJECTS = $(patsubst $(SRC)/%.cpp, $(OBJ)/%.o, $(SOURCES))
TARGETS = $(patsubst $(SRC)/%.cpp, $(BIN)/%,   $(SOURCES))

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
	$(CXX) -o $@ $^ $(LDFLAGS)

# object generation rule
$(OBJ)/%.o: $(SRC)/%.cpp | $(OBJ)/
	$(CXX) -c -I $(INC) $(CFLAGS) -o $@ $^

# clean rule
clean:
	@rm -rf $(BIN) $(OBJ)

