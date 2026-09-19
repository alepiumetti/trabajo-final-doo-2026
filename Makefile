CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17
LDFLAGS  = -lsqlite3

TARGET = ecogrid
SRCS   = main.cpp
OBJS   = $(SRCS:.cpp=.o)
HDRS   = GestionDatos.hpp types.hpp

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean run