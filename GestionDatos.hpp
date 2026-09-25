#ifndef GestionDatos_h
#define GestionDatos_h

#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "types.hpp"

struct DatoNodo {
  int id;
  std::string ubicacion;
  std::string tipo;
  double saldo;
  std::string perfil;
};

class gestionDatos {
private:
  sqlite3 *db = nullptr;

public:
  gestionDatos() {
    if (sqlite3_open("ejemplo.db", &db) != SQLITE_OK) {
      std::cerr << "No se pudo abrir la base de datos\n";
      throw std::runtime_error("Error al abrir la base de datos");
    }
  }

  ~gestionDatos() {
    if (db) sqlite3_close(db);
  }

  void crearTablas() {
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

    char *errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      std::cerr << "Error SQL: " << errMsg << "\n";
      sqlite3_free(errMsg);
      throw std::runtime_error("Error al ejecutar la consulta SQL");
    }

    // Tarifa base por defecto para todas las horas si no están definidas
    sqlite3_stmt *stmt = nullptr;
    const char *sqlTarifa = "INSERT OR IGNORE INTO CONFIG_TARIFAS "
                            "(hora, precio_base_kwh) VALUES (?, 1.0);";
    if (sqlite3_prepare_v2(db, sqlTarifa, -1, &stmt, nullptr) == SQLITE_OK) {
      for (int h = 0; h < 24; ++h) {
        sqlite3_bind_int(stmt, 1, h);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
      }
    }
    sqlite3_finalize(stmt);

    std::cout << "Tablas y trigger creados correctamente.\n";
  }

  std::vector<DatoNodo> cargarNodos(const std::string &rutaArchivo) {
    std::vector<DatoNodo> nodos;

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
      if (linea.empty()) continue;

      std::stringstream ss(linea);
      std::string campo;
      std::vector<std::string> columnas;
      while (getline(ss, campo, ',')) {
        columnas.push_back(campo);
      }

      // Formato CSV: id_nodo,ubicacion,tipo,saldo_cuenta,perfil_consumo
      if (columnas.size() < 4) {
        std::cerr << "Fila inválida (se omite): " << linea << std::endl;
        continue;
      }

      DatoNodo n;
      n.id = std::stoi(columnas[0]);
      n.ubicacion = columnas[1];
      n.tipo = columnas[2];
      n.saldo = std::stod(columnas[3]);
      n.perfil = (columnas.size() > 4) ? columnas[4] : "";

      nodos.push_back(n);

      const char *sqlInsert =
          "INSERT OR REPLACE INTO NODOS "
          "(id_nodo, ubicacion, tipo, saldo_cuenta, perfil_consumo) "
          "VALUES (?, ?, ?, ?, ?);";
      sqlite3_stmt *stmt = nullptr;
      if (sqlite3_prepare_v2(db, sqlInsert, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, n.id);
        sqlite3_bind_text(stmt, 2, n.ubicacion.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, n.tipo.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, n.saldo);
        if (n.perfil.empty())
          sqlite3_bind_null(stmt, 5);
        else
          sqlite3_bind_text(stmt, 5, n.perfil.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
      }
      sqlite3_finalize(stmt);
    }

    return nodos;
  }

  bool insertarTransaccion(const TransaccionEnergia &t) {
    const char *sql = "INSERT INTO TRANSACCIONES "
                      "(id_vendedor, id_comprador, kwh, precio_unitario) "
                      "VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      std::cerr << "Error preparando INSERT: " << sqlite3_errmsg(db) << "\n";
      return false;
    }

    sqlite3_bind_int(stmt, 1, t.idVendedor);
    sqlite3_bind_int(stmt, 2, t.idComprador);
    sqlite3_bind_double(stmt, 3, t.kwh);
    sqlite3_bind_double(stmt, 4, t.precio);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
      std::cerr << "No se pudo insertar la transacción: "
                << sqlite3_errmsg(db) << "\n";
      return false;
    }
    return true;
  }

  void actualizarSaldo(int idNodo, double nuevoSaldo) {
    const char *sql = "UPDATE NODOS SET saldo_cuenta = ? WHERE id_nodo = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_double(stmt, 1, nuevoSaldo);
      sqlite3_bind_int(stmt, 2, idNodo);
      sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
  }

  double leerTarifa(int hora) {
    const char *sql =
        "SELECT precio_base_kwh FROM CONFIG_TARIFAS WHERE hora = ?;";
    sqlite3_stmt *stmt = nullptr;
    double tarifa = 0.0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, hora);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        tarifa = sqlite3_column_double(stmt, 0);
      }
    }
    sqlite3_finalize(stmt);
    return tarifa;
  }

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
  }
};

#endif // GestionDatos_h
