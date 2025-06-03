# Compiler
CXX = g++

# Path to the directory that contains the 'evpp' header root
# This is where 'evpp/tcp_server.h' is found relative to.
# Since your main directory is 'pod', and 3rdparty is inside 'pod',
# this path correctly points to 'pod/3rdparty/evpp'.
EVPP_INCLUDE_ROOT = pod/3rdparty/evpp
JSON_INCLUDE_ROOT = pod/3rdparty/json/single_include
BASE64_INCLUDE_ROOT = pod/3rdparty/cpp-base64
 
# Path to the evpp library files (assuming evpp was built in its default 'build/lib' location)
# You need to ensure evpp itself has been compiled successfully.
EVPP_LIB_DIR = $(EVPP_INCLUDE_ROOT)/build/lib
BASE64_LIB_DIR = $(BASE64_INCLUDE_ROOT)
# Compiler flags
# -I$(EVPP_INCLUDE_ROOT) tells g++ to look in '3rdparty/evpp' for headers.
# So, '#include <evpp/tcp_server.h>' will correctly resolve to '3rdparty/evpp/evpp/tcp_server.h'.
CXXFLAGS = -Wall -g -I$(BASE64_INCLUDE_ROOT) -I$(JSON_INCLUDE_ROOT) -I$(EVPP_INCLUDE_ROOT)

# Linker flags
# These link your executable with the evpp library and its dependencies.
LDFLAGS = -L$(BASE64_LIB_DIR) -L$(EVPP_LIB_DIR) -levpp -lglog -lgflags -levent -lpthread  -lssl -lcrypto

# Target executable
TARGET = main

# For deleting the target and object files
TARGET_DEL = $(TARGET) $(OBJS)

# Source files
# Assuming server.cpp is directly inside the 'pod' main directory.
SRCS = $(wildcard pod/*.cpp)

# Object files
OBJS = $(SRCS:.cpp=.o)

# Default rule to build and run the executable
all: $(TARGET) run

# Rule to link object files into the target executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

# Rule to compile .cpp files into .o files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to run the executable
run: $(TARGET)
	./$(TARGET)

# Clean rule to remove generated files
clean:
	rm -f $(TARGET_DEL)