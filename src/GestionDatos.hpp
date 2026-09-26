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

// Resultado de persistirTick: si el tick quedó confirmado, cuántas
// transacciones y lecturas se escribieron, y el motivo en caso de rollback.
struct ResultadoTick {
  bool ok = true;           // el tick quedo confirmado
  size_t transacciones = 0; // transacciones enviadas a persistir
  size_t lecturas = 0;      // filas escritas en LECTURAS_HISTORICAS
  std::string motivo;       // motivo del rollback si !ok
};

class GestionDatos {
private:
  // Contexto que se le pasa al UDF actualizar_saldo_y_lecturas, que hace
  // de procedimiento almacenado.
  struct CtxProc {
    std::string *errUltimo = nullptr;
    std::string *tickHora = nullptr;
  };

  sqlite3 *db = nullptr;
  bool debug_ = false;

  // Último error con contexto (mensaje del rollback).
  std::string errUltimo_;

  // Timestamp simulado del tick en curso ("<fecha> HH:00:00"). Lo consume
  // el procedimiento actualizar_saldo_y_lecturas para sellar las lecturas.
  std::string tickHoraActual_;

  // Contexto que se le pasa al UDF (miembro porque SQLite guarda el
  // puntero hasta que la conexión se destruye).
  CtxProc ctxProc_;

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

  static std::string etiquetaNodo(int id) {
    return id == BATERIA ? std::string("Bateria") : std::to_string(id);
  }

  std::string motivoSQL(const std::string &contexto) {
    return contexto + ": " + sqlite3_errmsg(db);
  }

  // Revierte el bloque del tick y deja la conexión en un estado conocido.
  void deshacerTransaccion(const std::string &motivo) {
    std::cout << "[Rollback] Transacciones del tick rechazadas: " << motivo
              << "\n";
    abortarTransaccion();
  }

  // SQLite no tiene procedimientos almacenados: se registra una función SQL
  // de aplicación con la firma exacta del enunciado e se invoca desde C++
  // con la misma forma que en Oracle:
  //
  //   SELECT actualizar_saldo_y_lecturas(?, ?, ?, ?);
  //
  // p_tipo_operacion: 'compra' (descuenta saldo) | 'venta' (acumula).
  // Además del saldo, escribe la lectura histórica (una fila por operación).
  static void callbackActualizarSaldoYLecturas(sqlite3_context *ctx, int argc,
                                               sqlite3_value **argv) {
    // argc == 4: (p_id_nodo, p_kwh, p_precio, p_tipo_operacion)
    (void)argc;
    auto *c = static_cast<CtxProc *>(sqlite3_user_data(ctx));
    if (!c) {
      sqlite3_result_error(ctx, "sin contexto", -1);
      return;
    }

    const int idNodo = sqlite3_value_int(argv[0]);
    const double kwh = sqlite3_value_double(argv[1]);
    const double precio = sqlite3_value_double(argv[2]);
    const char *tipo =
        reinterpret_cast<const char *>(sqlite3_value_text(argv[3]));

    if (!tipo) {
      *c->errUltimo = "actualizar_saldo_y_lecturas: tipo_operacion nulo";
      sqlite3_result_error(ctx, "tipo_operacion nulo", -1);
      return;
    }

    const bool esCompra = std::string(tipo) == "compra";
    if (!esCompra && std::string(tipo) != "venta") {
      *c->errUltimo = std::string("actualizar_saldo_y_lecturas: tipo_operacion "
                                  "desconocida: ") +
                      tipo;
      sqlite3_result_error(ctx, "tipo_operacion invalida", -1);
      return;
    }

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    const double monto = kwh * precio;
    const char *sqlSaldo = esCompra ? "UPDATE NODOS SET saldo_cuenta = "
                                      "saldo_cuenta - ? WHERE id_nodo = ?;"
                                    : "UPDATE NODOS SET saldo_cuenta = "
                                      "saldo_cuenta + ? WHERE id_nodo = ?;";

    // 1) saldo_cuenta en NODOS (lo vigila el CHECK saldo_cuenta >= 0).
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sqlSaldo, -1, &stmt, nullptr) != SQLITE_OK) {
      *c->errUltimo =
          std::string("actualizar_saldo_y_lecturas: ") + sqlite3_errmsg(db);
      sqlite3_result_error(ctx, "fallo al preparar el ajuste de saldo", -1);
      return;
    }
    sqlite3_bind_double(stmt, 1, monto);
    sqlite3_bind_int(stmt, 2, idNodo);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
      *c->errUltimo =
          std::string("actualizar_saldo_y_lecturas (saldo del nodo ") +
          etiquetaNodo(idNodo) + "): " + sqlite3_errmsg(db);
      sqlite3_result_error(ctx, "fallo al ajustar el saldo", -1);
      return;
    }
    if (sqlite3_changes(db) == 0) {
      *c->errUltimo = "actualizar_saldo_y_lecturas: el nodo " +
                      etiquetaNodo(idNodo) + " no existe";
      sqlite3_result_error(ctx, "nodo inexistente", -1);
      return;
    }

    // 2) Lectura histórica: para una venta el nodo produjo kWh, para una
    // compra consumió kWh.
    const char *sqlLectura =
        "INSERT INTO LECTURAS_HISTORICAS (id_nodo, tick_hora, "
        "produccion_kwh, consumo_kwh, excedente_neto) "
        "VALUES (?, ?, ?, ?, ?);";
    const std::string &ts = (c->tickHora && !c->tickHora->empty())
                                ? *c->tickHora
                                : std::string("00");

    const double produccion = esCompra ? 0.0 : kwh;
    const double consumo = esCompra ? kwh : 0.0;
    const bool esBateria = (idNodo == BATERIA);
    const double excedente =
        esBateria ? (consumo - produccion) : (produccion - consumo);

    stmt = nullptr;
    if (sqlite3_prepare_v2(db, sqlLectura, -1, &stmt, nullptr) != SQLITE_OK) {
      *c->errUltimo =
          std::string("actualizar_saldo_y_lecturas: ") + sqlite3_errmsg(db);
      sqlite3_result_error(ctx, "fallo al preparar la lectura", -1);
      return;
    }
    sqlite3_bind_int(stmt, 1, idNodo);
    sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, produccion);
    sqlite3_bind_double(stmt, 4, consumo);
    sqlite3_bind_double(stmt, 5, excedente);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
      *c->errUltimo =
          std::string("actualizar_saldo_y_lecturas (lectura del nodo ") +
          etiquetaNodo(idNodo) + "): " + sqlite3_errmsg(db);
      sqlite3_result_error(ctx, "fallo al registrar la lectura", -1);
      return;
    }

    sqlite3_result_int(ctx, 1);
  }

  bool registrarProcedimientoAlmacenado() {
    ctxProc_.errUltimo = &errUltimo_;
    ctxProc_.tickHora = &tickHoraActual_;
    int rc = sqlite3_create_function(
        db, "actualizar_saldo_y_lecturas", 4, SQLITE_UTF8, &ctxProc_,
        callbackActualizarSaldoYLecturas, nullptr, nullptr);
    if (rc != SQLITE_OK) {
      std::cerr << "No se pudo registrar actualizar_saldo_y_lecturas: "
                << sqlite3_errmsg(db) << "\n";
      return false;
    }
    return true;
  }

  // Invoca el procedimiento almacenado (un lado de la transacción).
  bool invocarActualizarSaldoYLecturas(int idNodo, double kwh, double precio,
                                       const std::string &tipoOperacion) {
    if (tickHoraActual_.empty()) {
      errUltimo_ = "tick_hora sin valor: revisa 'fecha_simulada' en config.ini";
      return false;
    }
    const char *sql = "SELECT actualizar_saldo_y_lecturas(?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      errUltimo_ = motivoSQL("preparando actualizar_saldo_y_lecturas");
      return false;
    }
    sqlite3_bind_int(stmt, 1, idNodo);
    sqlite3_bind_double(stmt, 2, kwh);
    sqlite3_bind_double(stmt, 3, precio);
    sqlite3_bind_text(stmt, 4, tipoOperacion.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      if (errUltimo_.empty())
        errUltimo_ =
            std::string("actualizar_saldo_y_lecturas: ") + sqlite3_errmsg(db);
      return false;
    }
    return true;
  }

public:
  explicit GestionDatos(const Config &cfg) {
    if (sqlite3_open(cfg.dbPath.c_str(), &db) != SQLITE_OK) {
      std::cerr << "No se pudo abrir la base de datos: " << sqlite3_errmsg(db)
                << "\n";
      throw std::runtime_error("Error al abrir la base de datos");
    }
    // El procedimiento almacenado se registra por conexión, apenas se abre.
    if (!registrarProcedimientoAlmacenado()) {
      sqlite3_close(db);
      db = nullptr;
      throw std::runtime_error(
          "No se pudo registrar el procedimiento actualizar_saldo_y_lecturas");
    }
  }

  ~GestionDatos() {
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
  bool iniciarTransaccion() { return ejecutarSQL("BEGIN IMMEDIATE;"); }
  bool confirmarTransaccion() { return ejecutarSQL("COMMIT;"); }
  bool abortarTransaccion() { return ejecutarSQL("ROLLBACK;"); }

  // Persiste TODO el tick en un único bloque atómico.
  //
  //   BEGIN IMMEDIATE
  //     por cada transaccion:
  //       INSERT INTO TRANSACCIONES            -> dispara trg_validar_saldo
  //       SELECT actualizar_saldo_y_lecturas(vendedor, ..., 'venta')
  //       SELECT actualizar_saldo_y_lecturas(comprador, ..., 'compra')
  //   COMMIT
  //
  // Si algo falla (trg_validar_saldo, CHECK, FK) se revierte el bloque
  // completo y no queda NADA persistido de ese tick.
  ResultadoTick persistirTick(const std::vector<TransaccionEnergia> &trans,
                              const std::string &tickHora) {
    ResultadoTick r;
    r.transacciones = trans.size();
    tickHoraActual_ = tickHora;

    if (trans.empty())
      return r; // ok: no hay nada que persistir

    if (!iniciarTransaccion()) {
      r.motivo = std::string("no se pudo iniciar la transaccion: ") +
                 sqlite3_errmsg(db);
      r.ok = false;
      return r;
    }

    debugLog("[BD] BEGIN IMMEDIATE (bloque atomico del tick)");

    for (const auto &t : trans) {
      debugLog(
          "[BD] INSERT transaccion: vendedor=" + etiquetaNodo(t.idVendedor) +
          " comprador=" + etiquetaNodo(t.idComprador) + " kWh=" +
          std::to_string(t.kwh) + " precio=" + std::to_string(t.precio));

      if (!insertarTransaccion(t)) {
        r.motivo = motivoSQL("insertando la transaccion");
        deshacerTransaccion(r.motivo);
        r.ok = false;
        return r;
      }

      // Procedimiento almacenado: un lado por operación. El del vendedor
      // suma saldo (su lectura cuenta como producción); el del comprador
      // lo resta (su lectura cuenta como consumo).
      if (!invocarActualizarSaldoYLecturas(t.idVendedor, t.kwh, t.precio,
                                           "venta") ||
          !invocarActualizarSaldoYLecturas(t.idComprador, t.kwh, t.precio,
                                           "compra")) {
        r.motivo = errUltimo_;
        deshacerTransaccion(r.motivo);
        r.ok = false;
        return r;
      }

      r.lecturas += 2;
    }

    debugLog("[BD] COMMIT (" + std::to_string(trans.size()) +
             " transacciones, " + std::to_string(r.lecturas) + " lecturas)");
    if (!confirmarTransaccion()) {
      r.motivo = motivoSQL("confirmando la transaccion");
      deshacerTransaccion(r.motivo);
      r.ok = false;
      return r;
    }
    return r;
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
                      "(id_vendedor, id_comprador, kwh, precio_unitario, "
                      "tick_hora) "
                      "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      std::cout << "Error preparando INSERT: " << sqlite3_errmsg(db) << "\n";
      return false;
    }

    sqlite3_bind_int(stmt, 1, t.idVendedor);
    sqlite3_bind_int(stmt, 2, t.idComprador);
    sqlite3_bind_double(stmt, 3, t.kwh);
    sqlite3_bind_double(stmt, 4, t.precio);
    sqlite3_bind_int(stmt, 5, t.tickHora);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
      std::cout << "No se pudo insertar la transacción: " << sqlite3_errmsg(db)
                << "\n";
      return false;
    }
    return true;
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
      std::cerr << "No se pudo abrir: " << rutaArchivo << "\n";
      return filas;
    }

    std::string linea;
    bool esCabecera = true;
    while (getline(archivo, linea)) {
      if (esCabecera) {
        esCabecera = false;
        continue;
      }
      if (linea.empty())
        continue;

      std::stringstream ss(linea);
      std::string campo;
      std::vector<std::string> columnas;
      while (getline(ss, campo, ','))
        columnas.push_back(campo);

      // Formato CSV: id_orden,lado,id_nodo,kwh,precio
      if (columnas.size() < 5) {
        std::cerr << "Fila inválida (se omite): " << linea << std::endl;
        continue;
      }

      Orden o;
      try {
        o.idOrden = std::stoi(columnas[0]);
        o.esCompra =
            (columnas[1] == "compra"); // "compra" -> true, "venta" -> false
        o.idNodo = std::stoi(columnas[2]);
        o.kwh = std::stod(columnas[3]);
        o.precio = std::stod(columnas[4]);
      } catch (const std::exception &) {
        std::cerr << "Fila inválida (se omite): " << linea << std::endl;
        continue;
      }
      o.secuencia = 0; // la asigna GridManager::insertarOrden()

      filas.push_back(o);
    }

    return filas;
  }
};

#endif // GestionDatos_h
