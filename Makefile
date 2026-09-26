CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -g -O0 -Isrc
LDFLAGS  = -lsqlite3

TARGET = ecogrid
SRCS   = src/main.cpp
OBJS   = $(SRCS:.cpp=.o)
HDRS   = src/ecogrid.h src/GestionDatos.hpp src/types.hpp src/util/config.hpp

# Pruebas. test_motor ejercita el motor de subasta contra los casos de
# aceptación del PDF y no necesita base de datos; test_datos usa el esquema
# real sobre una base temporal aparte (no toca ejemplo.db).
TEST_SRCS = tests/test_motor.cpp tests/test_datos.cpp

ifeq ($(OS),Windows_NT)
EXEEXT = .exe
RM      = del /Q
MKDIR_P = if not exist
else
EXEEXT =
RM      = rm -f
MKDIR_P = mkdir -p
endif

TARGET_EXE = $(TARGET)$(EXEEXT)
TEST_BINS  = $(addsuffix $(EXEEXT),build/test_motor build/test_datos)

all: $(TARGET_EXE)

$(TARGET_EXE): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# build/test_motor no enlaza SQLite: el motor de subasta no depende de la
# base de datos, y que las pruebas lo demonstren evita que alguien meta SQL
# adentro por accidente.
build/test_motor$(EXEEXT): tests/test_motor.cpp $(HDRS)
	$(MKDIR_P) build
	$(CXX) $(CXXFLAGS) -o $@ $<

build/test_datos$(EXEEXT): tests/test_datos.cpp $(HDRS)
	$(MKDIR_P) build
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Las pruebas necesitan ./sql/crear_esquema.sql y ./datos en el directorio
# de trabajo, porque leen las rutas de config.ini tal como están.
test: $(TEST_BINS)
	@echo ""
	./build/test_motor$(EXEEXT)
	@echo ""
	./build/test_datos$(EXEEXT)

run: $(TARGET_EXE)
	./$(TARGET_EXE)

clean:
	$(RM) $(OBJS) $(TARGET_EXE) $(TEST_BINS)
	$(RM) test_datos.db
	$(RM) -r build

.PHONY: all clean run test
