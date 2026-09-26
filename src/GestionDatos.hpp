#ifndef GestionDatos_h
#define GestionDatos_h

#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "types.hpp"
#include "util/config.hpp"

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
  bool debug_ = false;

  // En modo debug y con stdin como terminal, espera Enter (paso a paso).
  void pausa() {
    if (debug_ && isatty(STDIN_FILENO)) {
      std::cout << "  (Enter para continuar)\n";
      std::cin.get();
    }
  }

  void debugLog(const std::string &msg) {
    if (!debug_)
      return;
    std::cout << msg << std::endl;
    pausa();
  }

  bool ejecutarSQL(const char *sql) {
    char *errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      std::cerr << "Error SQL: " << errMsg << "\n";
      sqlite3_free(errMsg);
      return false;
    }
    return true;
  }

public:
  explicit gestionDatos(const Config &cfg) {
    if (sqlite3_open(cfg.dbPath.c_str(), &db) != SQLITE_OK) {
      std::cerr << "No se pudo abrir la base de datos: " << sqlite3_errmsg(db)
                << "\n";
      throw std::runtime_error("Error al abrir la base de datos");
    }
  }

  ~gestionDatos() {
    if (db)
      sqlite3_close(db);
  }

  void setDebug(bool d) { debug_ = d; }

  // Ejecuta el script de creación de esquema desde sql/crear_esquema.sql
  void crearTablas(const Config &cfg) {
    std::ifstream archivo(cfg.sqlPath);
    if (!archivo.is_open()) {
      throw std::runtime_error("No se pudo abrir el script SQL: " +
                               cfg.sqlPath);
    }

    std::stringstream buffer;
    buffer << archivo.rdbuf();

    if (!ejecutarSQL(buffer.str().c_str())) {
      throw std::runtime_error("Error al ejecutar el esquema SQL");
    }

    std::cout << "Esquema y trigger creados correctamente.\n";
  }

  // ----------------------------------------------------------
  // Transaccionalidad por tick (BEGIN / COMMIT / ROLLBACK)
  // ----------------------------------------------------------
  bool iniciarTransaccion() { return ejecutarSQL("BEGIN;"); }
  bool confirmarTransaccion() { return ejecutarSQL("COMMIT;"); }
  bool abortarTransaccion() { return ejecutarSQL("ROLLBACK;"); }

  // Persiste TODAS las transacciones de un tick en un único bloque
  // atómico. Si alguna inserción falla (trigger, FK, constraint) se
  // ejecuta ROLLBACK y no queda NADA persistido de ese tick.
  bool persistirTransacciones(const std::vector<TransaccionEnergia> &trans) {
    if (trans.empty())
      return true;

    if (!iniciarTransaccion())
      return false;

    debugLog("[BD] BEGIN (bloque atómico del tick)");
    for (const auto &t : trans) {
    debugLog("[BD] INSERT transaccion: vendedor=" +
             (t.idVendedor == BATERIA ? std::string("Bateria")
                                      : std::to_string(t.idVendedor)) +
             " comprador=" +
             (t.idComprador == BATERIA ? std::string("Bateria")
                                       : std::to_string(t.idComprador)) +
             " kWh=" + std::to_string(t.kwh) +
             " precio=" + std::to_string(t.precio));
      if (!insertarTransaccion(t)) {
        std::string motivo = sqlite3_errmsg(db);
        abortarTransaccion();
        std::cerr << "[Rollback] Transacciones del tick rechazadas: " << motivo
                  << "\n";
        return false;
      }
    }

    debugLog("[BD] COMMIT");
    if (!confirmarTransaccion()) {
      abortarTransaccion();
      std::cerr << "[Rollback] Error en COMMIT: " << sqlite3_errmsg(db) << "\n";
      return false;
    }
    return true;
  }

  std::vector<DatoNodo> cargarNodosDesdeBD() {
    std::vector<DatoNodo> nodos;

    const char *sql = "SELECT id_nodo, ubicacion, tipo, saldo_cuenta, "
                      "perfil_consumo FROM NODOS ORDER BY id_nodo;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      std::cerr << "Error preparando SELECT NODOS: " << sqlite3_errmsg(db)
                << "\n";
      return nodos;
    }

    auto texto = [](const unsigned char *p) -> std::string {
      return p ? reinterpret_cast<const char *>(p) : std::string();
    };

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      DatoNodo n;
      n.id = sqlite3_column_int(stmt, 0);
      n.ubicacion = texto(sqlite3_column_text(stmt, 1));
      n.tipo = texto(sqlite3_column_text(stmt, 2));
      n.saldo = sqlite3_column_double(stmt, 3);
      n.perfil = texto(sqlite3_column_text(stmt, 4));
      nodos.push_back(n);
    }
    sqlite3_finalize(stmt);

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
      std::cerr << "No se pudo insertar la transacción: " << sqlite3_errmsg(db)
                << "\n";
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

  void insertarLectura(int idNodo, int hora, double produccion,
                       double consumo) {
    const char *sql = "INSERT INTO LECTURAS_HISTORICAS "
                      "(id_nodo, tick_hora, produccion_kwh, consumo_kwh, "
                      "excedente_neto) VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      std::cerr << "Error preparando INSERT lectura: " << sqlite3_errmsg(db)
                << "\n";
      return;
    }

    std::string horaStr =
        (hora < 10) ? "0" + std::to_string(hora) : std::to_string(hora);
    double excedente = produccion - consumo;

    sqlite3_bind_int(stmt, 1, idNodo);
    sqlite3_bind_text(stmt, 2, horaStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, produccion);
    sqlite3_bind_double(stmt, 4, consumo);
    sqlite3_bind_double(stmt, 5, excedente);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      std::cerr << "No se pudo insertar la lectura: " << sqlite3_errmsg(db)
                << "\n";
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
