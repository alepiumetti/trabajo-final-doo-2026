#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "types.hpp"

static int callback(void *NotUsed, int argc, char **argv, char **azColName) {
  std::cout << "Nodos creados: \n";
  for (int i = 0; i < argc; i++) {
    std::cout << azColName[i] << ": " << (argv[i] ? argv[i] : "NULL") << "\n";
  }
  std::cout << "\n";
  return 0;
}

class gestionDatos {
public:
  gestionDatos() {};
  ~gestionDatos() {};

  void crearTablas(sqlite3 *db) {
    if (sqlite3_open("ejemplo.db", &db) != SQLITE_OK) {
      std::cerr << "No se pudo abrir la base de datos\n";
      throw std::runtime_error("Error al abrir la base de datos");
    }

    const char *select_sql = "SELECT * FROM NODOS;";
    char *errMsg = nullptr;

    if (sqlite3_exec(db, select_sql, callback, nullptr, &errMsg) != SQLITE_OK) {
      std::cerr << "Error SQL: " << errMsg << "\n";
      sqlite3_free(errMsg);
      sqlite3_close(db);

      throw std::runtime_error("Error al ejecutar la consulta SQL");
    }

    if (sqlite3_open("ejemplo.db", &db) != SQLITE_OK) {
      std::cerr << "No se pudo abrir la base de datos\n";
      throw std::runtime_error("Error al abrir la base de datos");
    } else {
      std::cout << "Base de datos abierta correctamente.\n";
    }

    const char *sql = R"SQL(
    PRAGMA foreign_keys = ON;

    CREATE TABLE IF NOT EXISTS NODOS (
      id_nodo INTEGER PRIMARY KEY,
      ubicacion TEXT NOT NULL,
      tipo TEXT NOT NULL CHECK (tipo IN ('Consumidor', 'Prosumidor', 'Bateria')),
      saldo_cuenta REAL DEFAULT 0 CHECK (saldo_cuenta >= 0),
      perfil_consumo TEXT
    );

    CREATE TABLE IF NOT EXISTS LECTURAS_HISTORICAS (
      id_lectura INTEGER PRIMARY KEY AUTOINCREMENT,
      id_nodo INTEGER NOT NULL,
      tick_hora TEXT NOT NULL,
      produccion_kwh REAL DEFAULT 0,
      consumo_kwh REAL DEFAULT 0,
      excedente_neto REAL,
      FOREIGN KEY (id_nodo) REFERENCES NODOS(id_nodo)
    );

    CREATE TABLE IF NOT EXISTS TRANSACCIONES (
      id_transaccion INTEGER PRIMARY KEY AUTOINCREMENT,
      id_vendedor INTEGER NOT NULL,
      id_comprador INTEGER NOT NULL,
      kwh REAL NOT NULL CHECK (kwh > 0),
      precio_unitario REAL NOT NULL CHECK (precio_unitario > 0),
      fecha_transaccion TEXT DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY (id_vendedor) REFERENCES NODOS(id_nodo),
      FOREIGN KEY (id_comprador) REFERENCES NODOS(id_nodo)
    );

    CREATE TABLE IF NOT EXISTS CONFIG_TARIFAS (
      hora INTEGER PRIMARY KEY CHECK (hora BETWEEN 0 AND 23),
      precio_base_kwh REAL NOT NULL
    );

    CREATE TRIGGER IF NOT EXISTS trg_validar_saldo
    BEFORE INSERT ON TRANSACCIONES
    FOR EACH ROW
    BEGIN
      SELECT CASE
        WHEN (
          SELECT saldo_cuenta
          FROM NODOS
          WHERE id_nodo = NEW.id_comprador
        ) < (NEW.kwh * NEW.precio_unitario)
        THEN RAISE(ABORT, 'Saldo insuficiente para realizar la compra')
      END;
    END;
    )SQL";

    // INSERT INTO NODOS (id_nodo, ubicacion, tipo, saldo_cuenta,
    // perfil_consumo) VALUES (1, 'Residencial A', 'Consumidor', 100.0,
    // 'Residencial'), (2, 'Comercial B', 'Consumidor', 200.0, 'Comercial'), (3,
    // 'Industrial C', 'Consumidor', 300.0, 'Industrial'), (4, 'Prosumidor D',
    // 'Prosumidor', 150.0, NULL), (5, 'Bateria E', 'Bateria', 50.0, NULL);

    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      std::cerr << "Error SQL: " << errMsg << "\n";
      sqlite3_free(errMsg);
      sqlite3_close(db);

      throw std::runtime_error("Error al ejecutar la consulta SQL");
    }
    sqlite3_close(db);
    std::cout << "Tablas y trigger creados correctamente.\n";
  };

  static std::vector<Orden> leerCSV(const std::string &rutaArchivo) {
    std::vector<Orden> filas;

    std::ifstream archivo(rutaArchivo);

    if (!archivo.is_open()) {
      throw std::runtime_error("No se pudo abrir: " + rutaArchivo);
    }

    std::string linea;
    bool esCabecera = true;
    while (getline(archivo, linea)) {
      if (esCabecera) {
        esCabecera = false;
        continue;
      }

      std::stringstream ss(linea);
      std::string campo;
      std::vector<std::string> columnas;
      while (getline(ss, campo, ',')) {
        columnas.push_back(campo);
      }

      // Formato CSV: id_orden,lado,id_nodo,kwh,precio
      if (columnas.size() < 5) {
        std::cerr << "Fila inválida (se omite): " << linea << std::endl;
        continue;
      }

      Orden o;
      o.idOrden = std::stoi(columnas[0]);
      o.esCompra =
          (columnas[1] == "compra"); // "compra" -> true, "venta" -> false
      o.idNodo = std::stoi(columnas[2]);
      o.kwh = std::stod(columnas[3]);
      o.precio = std::stod(columnas[4]);
      o.secuencia = 0; // la asigna GridManager.insertarOrden()

      filas.push_back(o);
    }

    return filas;
  };
};
