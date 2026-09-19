#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "GestionDatos.hpp"
#include "types.hpp"

// ==========================================================
// Métodos auxiliares del procesamiento por tick
// (declarados; la lógica se completa en el próximo paso)
// ==========================================================

// Ruta del CSV del tick: ./datos/ofertas_HH.csv (HH = hora a 2 dígitos)
std::string rutaTick(int hora);

// Procesa el tick completo: matching P2P, transferencia a batería
// y limpieza del libro al final.
void procesarTick(GridManager& gm, NodoAlmacenamiento& bateria,
                  double precioBaseHora);

// Precio base de la hora, leído de CONFIG_TARIFAS.
double obtenerPrecioBase(sqlite3* db, int hora);

// Persiste las transacciones del tick en la tabla TRANSACCIONES.
void persistirTransacciones(sqlite3* db,
                            const std::vector<TransaccionEnergia>& transacciones);

// Carga los nodos (consumidor/prosumidor/batería) desde la tabla NODOS.
void cargarNodos(sqlite3* db, std::vector<std::unique_ptr<NodoRed>>& nodos);

// ----------------------------------------------------------
int main() {
  gestionDatos gestor;
  sqlite3* db = nullptr;

  std::cout << "Creando tablas SQLite3...\n";
  gestor.crearTablas(db);

  // Conexión propia para las consultas del tick (crearTablas cierra la suya).
  if (sqlite3_open("ejemplo.db", &db) != SQLITE_OK) {
    std::cerr << "No se pudo abrir la base de datos: " << sqlite3_errmsg(db)
              << "\n";
    sqlite3_close(db);
    return 1;
  }

  // TODO: nodos y batería deberían salir de cargarNodos(db, nodos).
  std::vector<std::unique_ptr<NodoRed>> nodos;
  cargarNodos(db, nodos);

  // Batería del sistema (por ahora una instancia por defecto).
  NodoAlmacenamiento bateria(0, "Bateria principal");

  // Cada tick (hora 0..23) se procesa de forma INDEPENDIENTE:
  // cada tick tiene su propio libro de órdenes y sus transacciones.
  for (int hora = 0; hora < 24; ++hora) {
    std::cout << "\n=== Tick hora " << hora << " ===\n";

    GridManager gm([](const std::string& msg) { std::cout << msg << "\n"; });
    gm.setTickActual(hora);

    double precioBaseHora = obtenerPrecioBase(db, hora);

    // La oferta de la batería se inserta ANTES de las órdenes del CSV.
    gm.insertarOfertaBateria(bateria.getId(),
                             bateria.calcularExcedente(), precioBaseHora);

    // Órdenes del tick (un CSV por tick).
    std::vector<Orden> ordenes = gestor.leerCSV(rutaTick(hora));
    for (const auto& orden : ordenes) {
      gm.insertarOrden(orden);
    }

    // Transacciones del tick.
    procesarTick(gm, bateria, precioBaseHora);
    persistirTransacciones(db, gm.getTransacciones());
  }

  sqlite3_close(db);
  return 0;
}

// ==========================================================
// Definiciones (stubs — por completar)
// ==========================================================

std::string rutaTick(int hora) {
  std::ostringstream ss;
  ss << "./datos/ofertas_" << std::setw(2) << std::setfill('0') << hora
     << ".csv";
  return ss.str();
}

void procesarTick(GridManager& gm, NodoAlmacenamiento& bateria,
                  double precioBaseHora) {
  // 1. Transacciones P2P (loguea cada una).
  gm.ejecutarMatching();

  // 2. Excedentes no vendidos -> batería (loguea cada absorción).
  gm.transferirExcedentesABateria(bateria, precioBaseHora);

  // 3. Remanentes: log de demanda insatisfecha y excedente no absorbido.
  gm.limpiarLibroAlFinalDelTick();
}

double obtenerPrecioBase(sqlite3* db, int hora) {
  if (db == nullptr) {
    std::cerr << "obtenerPrecioBase: base de datos no abierta.\n";
    return 0.0;
  }

  const char* sql = "SELECT precio_base_kwh FROM CONFIG_TARIFAS WHERE hora = ?;";
  sqlite3_stmt* stmt = nullptr;

  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "Error preparando CONFIG_TARIFAS: " << sqlite3_errmsg(db)
              << "\n";
    return 0.0;
  }

  sqlite3_bind_int(stmt, 1, hora);

  double precio = 0.0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    precio = sqlite3_column_double(stmt, 0);
  } else {
    std::cerr << "Sin precio base definido para la hora " << hora
              << " (CONFIG_TARIFAS vacía?), se usa 0.0\n";
  }

  sqlite3_finalize(stmt);
  return precio;
}

void persistirTransacciones(sqlite3* db,
                            const std::vector<TransaccionEnergia>& transacciones) {
  // TODO: INSERT INTO TRANSACCIONES
  //       (id_vendedor, id_comprador, kwh, precio_unitario) VALUES ...;
  (void)db;
  (void)transacciones;
}

void cargarNodos(sqlite3* db, std::vector<std::unique_ptr<NodoRed>>& nodos) {
  // TODO: leer NODOS y construir NodoConsumidor / NodoProsumidor /
  //       NodoAlmacenamiento según el tipo de cada fila.
  (void)db;
  (void)nodos;
}