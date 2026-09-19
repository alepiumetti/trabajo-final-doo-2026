#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "GestionDatos.hpp"
#include "types.hpp"

int main() {

  gestionDatos gestor;

  sqlite3 *db = nullptr;

  std::cout << "Crear tablas SQLite3";
  gestor.crearTablas(db);
  std::cout << "Tablas creadas";

  auto filas = gestor.leerCSV("./datos/ofertas_01.csv");

  return 0;
}
