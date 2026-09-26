CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -Isrc
LDFLAGS  = -lsqlite3

TARGET = ecogrid
SRCS   = src/main.cpp
OBJS   = $(SRCS:.cpp=.o)
HDRS   = src/GestionDatos.hpp src/types.hpp src/util/config.hpp

ifeq ($(OS),Windows_NT)
EXEEXT = .exe
RM      = del /Q
else
EXEEXT =
RM      = rm -f
endif

TARGET_EXE = $(TARGET)$(EXEEXT)

all: $(TARGET_EXE)

$(TARGET_EXE): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET_EXE)
	./$(TARGET_EXE)

clean:
	$(RM) $(OBJS) $(TARGET_EXE)

.PHONY: all clean run