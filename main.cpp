#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "types.hpp"

#include "GestionDatos.hpp"

int main() {

  gestionDatos gestor;

  sqlite3 *db = nullptr;

  gestor.crearTablas(db);

  auto ordenes = gestor.leerCSV("./datos/ofertas_01.csv");

  for (auto &o : ordenes) {
    std::cout << "Orden " << o.idOrden << " - " << "Nodo " << o.idNodo << " - "
              << (o.esCompra ? "compra" : "venta") << " " << o.kwh << " kWh a $"
              << o.precio << std::endl;
  }

  return 0;
}
