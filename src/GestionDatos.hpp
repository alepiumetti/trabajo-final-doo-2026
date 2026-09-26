#ifndef GestionDatos_h
#define GestionDatos_h

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
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

// Resultado de persistirTick. Devolver una estructura (en vez de un bool)
// permite que main.cpp sepa quantas lecturas historicas escribio el
// procedimiento almacenado y por que motivo se revirtio el tick, sin
// tener que consultarlo por otro lado.
struct ResultadoTick {
  bool ok = true;                 // el tick quedo confirmado
  size_t transacciones = 0;       // transacciones enviadas a persistir
  size_t lecturas = 0;            // filas escritas en LECTURAS_HISTORICAS
  std::string motivo;             // motivo del rollback si !ok
};

class gestionDatos {
private:
  // Contexto que se le pasa al UDF actualizar_saldo_y_lecturas, que hace
  // de procedimiento almacenado (sección 4.2 del PDF). Se declara acá
  // porque los miembros de más abajo lo usan.
  struct CtxProc {
    std::string *errUltimo = nullptr;
    std::string *tickHora = nullptr;
  };

  sqlite3 *db = nullptr;
  bool debug_ = false;

  // Último error con contexto. SQLite no diferencia "el nodo no existe" de
  // un error de SQL, y el mensaje de rollback necesita algo útil.
  std::string errUltimo_;

  // Timestamp simulado del tick en curso ("<fecha> HH:00:00"). Lo consume
  // el procedimiento actualizar_saldo_y_lecturas para sellar las lecturas
  // históricas con la misma hora que el tick.
  std::string tickHoraActual_;

  // Contexto que se le pasa al UDF actualizar_saldo_y_lecturas. Es un
  // miembro (no un local) porque SQLite guarda el puntero hasta que la
  // conexión se destruye.
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

  static bool coincide(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(a));
  }

  // Los motivos que abortan un tick (trigger, CHECK, FK, UNIQUE) llegan
  // todos como SQLITE_CONSTRAINT: sólo el código extendido los distingue.
  // Sirve para que el log diga de dónde vino el rechazo y se pueda
  // grepear ("[Rollback] ... Trigger: ...").
  std::string origenDelError() {
    if (sqlite3_errcode(db) != SQLITE_CONSTRAINT)
      return "SQL";
    switch (sqlite3_extended_errcode(db)) {
    case SQLITE_CONSTRAINT_TRIGGER:
      return "Trigger";
    case SQLITE_CONSTRAINT_CHECK:
      return "CHECK";
    case SQLITE_CONSTRAINT_FOREIGNKEY:
      return "FK";
    case SQLITE_CONSTRAINT_NOTNULL:
      return "NOT NULL";
    case SQLITE_CONSTRAINT_UNIQUE:
    case SQLITE_CONSTRAINT_PRIMARYKEY:
      return "UNIQUE";
    default:
      return "constraint";
    }
  }

  std::string motivoSQL(const std::string &contexto) {
    return origenDelError() + ": " + contexto + ": " + sqlite3_errmsg(db);
  }

  // Lee todos los saldos de NODOS (snapshot del bloque o verificación).
  bool leerSaldos(std::map<int, double> &saldos) {
    saldos.clear();
    const char *sql = "SELECT id_nodo, saldo_cuenta FROM NODOS;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      errUltimo_ = motivoSQL("preparando la lectura de saldos");
      return false;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      saldos[sqlite3_column_int(stmt, 0)] = sqlite3_column_double(stmt, 1);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      errUltimo_ = motivoSQL("leyendo los saldos");
      return false;
    }
    return true;
  }


  // Revierte el bloque del tick y deja la conexión en un estado conocido.
  // Va por stdout (y no por stderr) porque un rollback es un evento de la
  // simulación, no un fallo del programa: si el log se captura redirigiendo
  // sólo stdout, el rechazo tiene que verse igual que el resto del tick.
  void deshacerTransaccion(const std::string &motivo) {
    std::cout << "[Rollback] Transacciones del tick rechazadas: " << motivo
              << "\n";
    if (!abortarTransaccion()) {
      std::cout << "[Rollback] Falló el ROLLBACK: " << sqlite3_errmsg(db)
                << " (estado de la conexión desconocido)\n";
    }
    if (sqlite3_get_autocommit(db) == 0) {
      std::cout << "[Rollback] La conexión sigue en una transacción abierta: "
                   "las escrituras siguientes no se confirmarían.\n";
    }
  }

  // Destructor del contexto de la función SQL. sqlite3 lo llama cuando se
  // cierra la conexión o se re-registra la función.
  static void udfError(void *p) {
    auto *c = static_cast<CtxProc *>(p);
    if (c && c->errUltimo)
      *c->errUltimo = "actualizar_saldo_y_lecturas: error interno";
  }

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
    const char *tipo = reinterpret_cast<const char *>(sqlite3_value_text(argv[3]));

    if (!tipo) {
      *c->errUltimo = "actualizar_saldo_y_lecturas: tipo_operacion nulo";
      sqlite3_result_error(ctx, "tipo_operacion nulo", -1);
      return;
    }

    const bool esCompra = std::string(tipo) == "compra";
    if (!esCompra && std::string(tipo) != "venta") {
      *c->errUltimo = std::string("actualizar_saldo_y_lecturas: tipo_operacion "
                                  "desconocida: ") + tipo;
      sqlite3_result_error(ctx, "tipo_operacion invalida", -1);
      return;
    }

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    const double monto = kwh * precio;
    const char *sqlSaldo =
        esCompra ? "UPDATE NODOS SET saldo_cuenta = saldo_cuenta - ? WHERE id_nodo = ?;"
                 : "UPDATE NODOS SET saldo_cuenta = saldo_cuenta + ? WHERE id_nodo = ?;";

    // 1) saldo_cuenta en NODOS (lo vigila el CHECK saldo_cuenta >= 0).
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sqlSaldo, -1, &stmt, nullptr) != SQLITE_OK) {
      *c->errUltimo = std::string("actualizar_saldo_y_lecturas: ") +
                      sqlite3_errmsg(db);
      sqlite3_result_error(ctx, "fallo al preparar el ajuste de saldo", -1);
      return;
    }
    sqlite3_bind_double(stmt, 1, monto);
    sqlite3_bind_int(stmt, 2, idNodo);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
      *c->errUltimo = std::string("actualizar_saldo_y_lecturas (saldo del nodo ") +
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
    const std::string &ts =
        (c->tickHora && !c->tickHora->empty()) ? *c->tickHora : std::string("00");

    const double produccion = esCompra ? 0.0 : kwh;
    const double consumo = esCompra ? kwh : 0.0;
    const bool esBateria = (idNodo == BATERIA);
    const double excedente =
        esBateria ? (consumo - produccion) : (produccion - consumo);

    stmt = nullptr;
    if (sqlite3_prepare_v2(db, sqlLectura, -1, &stmt, nullptr) != SQLITE_OK) {
      *c->errUltimo = std::string("actualizar_saldo_y_lecturas: ") +
                      sqlite3_errmsg(db);
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
      *c->errUltimo = std::string("actualizar_saldo_y_lecturas (lectura del nodo ") +
                      etiquetaNodo(idNodo) + "): " + sqlite3_errmsg(db);
      sqlite3_result_error(ctx, "fallo al registrar la lectura", -1);
      return;
    }

    sqlite3_result_int(ctx, 1);
  }

  bool registrarProcedimientoAlmacenado() {
    ctxProc_.errUltimo = &errUltimo_;
    ctxProc_.tickHora = &tickHoraActual_;
    int rc = sqlite3_create_function_v2(
        db, "actualizar_saldo_y_lecturas", 4, SQLITE_UTF8, &ctxProc_,
        callbackActualizarSaldoYLecturas, nullptr, nullptr, udfError);
    if (rc != SQLITE_OK) {
      std::cerr << "No se pudo registrar actualizar_saldo_y_lecturas: "
                << sqlite3_errmsg(db) << "\n";
      return false;
    }
    return true;
  }


  // Si el BEGIN falla porque quedó una transacción abierta de un tick
  // anterior, las escrituras siguientes (autocommit) quedarían colgando y
  // nunca se confirmarían: se revierte antes de seguir.
  void asegurarSinTransaccion() {
    if (sqlite3_get_autocommit(db) != 0)
      return;
    std::cout << "[Transaccion] Quedó una transacción abierta del tick "
                 "anterior; se revierte.\n";
    if (!abortarTransaccion()) {
      std::cout << "[Transaccion] No se pudo cerrar: " << sqlite3_errmsg(db)
                << "\n";
    }
  }

public:
  explicit gestionDatos(const Config &cfg) {
    if (sqlite3_open(cfg.dbPath.c_str(), &db) != SQLITE_OK) {
      std::cerr << "No se pudo abrir la base de datos: " << sqlite3_errmsg(db)
                << "\n";
      throw std::runtime_error("Error al abrir la base de datos");
    }
    // El procedimiento almacenado se registra por conexión, apenas se
    // abre: si fallara, el tick no podría actualizar saldos ni lecturas.
    if (!registrarProcedimientoAlmacenado()) {
      sqlite3_close(db);
      db = nullptr;
      throw std::runtime_error(
          "No se pudo registrar el procedimiento actualizar_saldo_y_lecturas");
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

  // Invoca el procedimiento almacenado con la firma exacta que pide el
  // PDF. Es público para poder ejercitarlo desde las pruebas; el flujo
  // normal lo llama persistirTick, una vez por cada lado de la
  // transacción.
  bool invocarActualizarSaldoYLecturas(int idNodo, double kwh, double precio,
                                       const std::string &tipoOperacion) {
    if (tickHoraActual_.empty()) {
      errUltimo_ =
          "tick_hora sin valor: revisa 'fecha_simulada' en config.ini";
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
        errUltimo_ = std::string("actualizar_saldo_y_lecturas: ") +
                     sqlite3_errmsg(db);
      return false;
    }
    return true;
  }

  // ----------------------------------------------------------
  // Transaccionalidad por tick (BEGIN / COMMIT / ROLLBACK)
  // ----------------------------------------------------------
  // BEGIN IMMEDIATE toma el lock de escritura desde el BEGIN: el conflicto
  // de concurrencia aparece al inicio del tick y no a mitad del lote de
  // INSERTs, con el bloque a medio hacer.
  bool iniciarTransaccion() { return ejecutarSQL("BEGIN IMMEDIATE;"); }
  bool confirmarTransaccion() { return ejecutarSQL("COMMIT;"); }
  bool abortarTransaccion() { return ejecutarSQL("ROLLBACK;"); }

  // Persiste TODO el tick en un único bloque atómico. El orden es el
  // que pide el PDF (secciones 3.2 y 4.2):
  //
  //   BEGIN IMMEDIATE
  //     por cada transaccion:
  //       INSERT INTO TRANSACCIONES            -> dispara trg_validar_saldo
  //       SELECT actualizar_saldo_y_lecturas(vendedor, ..., 'venta')
  //       SELECT actualizar_saldo_y_lecturas(comprador, ..., 'compra')
  //   COMMIT
  //
  // El INSERT va antes del ajuste de saldo a proposito: asi el trigger
  // valida contra el saldo ya descontado por las transacciones
  // anteriores del mismo tick, que es lo mismo que ve el motor de
  // matching en memoria.
  //
  // Las lecturas historicas las escribe el procedimiento almacenado, no
  // el llamador: una fila por operacion (venta o compra), que es la
  // granularidad que el enunciado describe ("se ejecutara despues de
  // cada transaccion confirmada").
  //
  // Si algo falla (trg_validar_saldo, CHECK saldo >= 0, FK) se revierte
  // el bloque completo y no queda NADA persistido de ese tick.
  //
  // `saldos` es el estado del modelo de dominio al cierre del tick: se
  // usa para verificar que la memoria y la BD rendan lo mismo antes de
  // confirmar.
  ResultadoTick persistirTick(const std::vector<TransaccionEnergia> &trans,
                              const std::map<int, double> &saldos,
                              const std::string &tickHora) {
    ResultadoTick r;
    r.transacciones = trans.size();
    tickHoraActual_ = tickHora;

    if (trans.empty())
      return r; // ok: no hay nada que persistir

    if (!iniciarTransaccion()) {
      asegurarSinTransaccion();
      r.motivo = std::string("no se pudo iniciar la transaccion: ") +
                 sqlite3_errmsg(db);
      r.ok = false;
      return r;
    }

    debugLog("[BD] BEGIN IMMEDIATE (bloque atomico del tick)");

    // Foto de los saldos: permite verificar antes del COMMIT que el saldo
    // final es exactamente el inicial + el movimiento del ledger del tick.
    std::map<int, double> saldoInicial;
    if (!leerSaldos(saldoInicial)) {
      r.motivo = errUltimo_;
      deshacerTransaccion(r.motivo);
      r.ok = false;
      return r;
    }
    std::map<int, double> esperado = saldoInicial;

    for (const auto &t : trans) {
      debugLog("[BD] INSERT transaccion: vendedor=" +
               etiquetaNodo(t.idVendedor) + " comprador=" +
               etiquetaNodo(t.idComprador) + " kWh=" + std::to_string(t.kwh) +
               " precio=" + std::to_string(t.precio));

      if (!insertarTransaccion(t)) {
        r.motivo = motivoSQL("insertando la transaccion");
        deshacerTransaccion(r.motivo);
        r.ok = false;
        return r;
      }

      // Procedimiento almacenado: un lado por operacion. El del
      // vendedor suma saldo (y su lectura cuenta como produccion), el
      // del comprador lo resta (y su lectura cuenta como consumo).
      const double monto = t.kwh * t.precio;
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
      esperado[t.idVendedor] += monto;
      esperado[t.idComprador] -= monto;
    }

    // Verificacion de integridad antes del COMMIT: ni la BD ni el modelo en
    // memoria pueden quedar con un saldo que el ledger del tick no explique.
    std::map<int, double> saldoFinal;
    if (!leerSaldos(saldoFinal)) {
      r.motivo = errUltimo_;
      deshacerTransaccion(r.motivo);
      r.ok = false;
      return r;
    }
    for (const auto &[id, saldo] : saldoFinal) {
      const std::string nodo = etiquetaNodo(id);
      if (!coincide(saldo, esperado[id])) {
        r.motivo = "saldo inconsistente del nodo " + nodo + ": BD=" +
                   std::to_string(saldo) + ", ledger=" +
                   std::to_string(esperado[id]);
        deshacerTransaccion(r.motivo);
        r.ok = false;
        return r;
      }
      auto it = saldos.find(id);
      if (it == saldos.end() || !coincide(it->second, esperado[id])) {
        const double enMemoria = it == saldos.end() ? 0.0 : it->second;
        r.motivo = "el modelo en memoria del nodo " + nodo +
                   " no coincide con el ledger del tick: memoria=" +
                   std::to_string(enMemoria) + ", ledger=" +
                   std::to_string(esperado[id]);
        deshacerTransaccion(r.motivo);
        r.ok = false;
        return r;
      }
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

  // ------------------------------------------------------------------
  // Trigger de prueba (opt-in con --trigger-prueba)
  // ------------------------------------------------------------------
  // trg_validar_saldo es la última red de seguridad y, con los CSV de
  // fábrica, nunca llega a dispararse: el motor de matching ya rechaza en
  // memoria toda compra que el saldo no cubre, y aplica exactamente el
  // mismo predicado sobre el mismo saldo. Para poder ejercitar el ROLLBACK
  // de punta a punta hace falta una regla MÁS estricta, que es lo que
  // instala este trigger: rechaza toda compra individual por encima de
  // `limiteCreditos`.
  void activarTriggerPrueba(double limiteCreditos) {
    const std::string limite = std::to_string(limiteCreditos);
    const std::string sql =
        "CREATE TRIGGER IF NOT EXISTS trg_prueba_saldo "
        "BEFORE INSERT ON TRANSACCIONES FOR EACH ROW BEGIN "
        "  SELECT CASE WHEN (NEW.kwh * NEW.precio_unitario) > " + limite +
        "  THEN RAISE(ABORT, 'Trigger de prueba: compra individual de " +
        limite + " creditos o mas') END; END;";
    if (!ejecutarSQL(sql.c_str())) {
      std::cerr << "No se pudo activar el trigger de prueba.\n";
      return;
    }
    std::cout << "=== TRIGGER DE PRUEBA ACTIVO: se rechazan compras "
                 "individuales de mas de " << limite << " creditos ===\n";
  }

  // Si no se pidió el trigger de prueba se borra el que haya quedado de una
  // corrida anterior: crearTablas usa IF NOT EXISTS, así que persistiría
  // y dispararía en las corridas siguientes sin haberlo pedido.
  void desactivarTriggerPrueba() {
    ejecutarSQL("DROP TRIGGER IF EXISTS trg_prueba_saldo;");
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

  // ------------------------------------------------------------------
  // Lectura de ofertas_HH.csv (sección 6.1 del PDF)
  // ------------------------------------------------------------------
  // Formato: id_orden,lado,id_nodo,kwh,precio
  //
  // Devuelve un informe con las filas válidas y las rechazadas. Antes,
  // un valor no numérico (kwh=abc) o un archivo faltante lanzaba una
  // excepción sin capturar y mataba el proceso con SIGABRT, perdiendo
  // toda la simulación. Ahora el CSV defectuoso se reporta y la
  // simulación sigue.
  struct InformeCSV {
    std::vector<Orden> filas;
    std::vector<std::string> rechazadas;
  };

  // Convierte texto a número devolviendo false si no es válido o si tiene
  // basura alrededor. std::stod acepta sufijos ("5kg" -> 5) y lanza
  // excepción si no hay número; esto la hace explícita.
  static bool parsearDouble(const std::string &texto, double &salida) {
    if (texto.empty())
      return false;
    try {
      size_t pos = 0;
      double valor = std::stod(texto, &pos);
      // Todo lo que sobra ("5kg", "3,5" mal partido) es un error de formato.
      while (pos < texto.size() &&
             (texto[pos] == ' ' || texto[pos] == '\t' || texto[pos] == '\r'))
        ++pos;
      if (pos != texto.size())
        return false;
      if (!std::isfinite(valor))
        return false;
      salida = valor;
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  static bool parsearEntero(const std::string &texto, int &salida) {
    double valor = 0.0;
    if (!parsearDouble(texto, valor))
      return false;
    if (valor != std::floor(valor))
      return false;
    if (valor < -2147483648.0 || valor > 2147483647.0)
      return false;
    salida = static_cast<int>(valor);
    return true;
  }

  static InformeCSV leerCSVConInforme(const std::string &rutaArchivo) {
    InformeCSV informe;

    std::ifstream archivo(rutaArchivo);
    if (!archivo.is_open()) {
      informe.rechazadas.push_back("archivo inexistente o ilegible: " +
                                    rutaArchivo);
      return informe;
    }

    std::string linea;
    int numeroLinea = 0;
    while (getline(archivo, linea)) {
      ++numeroLinea;
      // Tolera CRLF y lignes en blanco.
      while (!linea.empty() &&
             (linea.back() == '\r' || linea.back() == ' ' || linea.back() == '\t'))
        linea.pop_back();
      if (linea.empty())
        continue;
      // Cabecera: se salta la primera linea con texto no numerico.
      if (linea.rfind("id_orden", 0) == 0)
        continue;

      std::stringstream ss(linea);
      std::string campo;
      std::vector<std::string> columnas;
      while (getline(ss, campo, ','))
        columnas.push_back(campo);

      auto rechazar = [&](const std::string &motivo) {
        informe.rechazadas.push_back("linea " + std::to_string(numeroLinea) +
                                      ": " + motivo + " -> " + linea);
      };

      if (columnas.size() < 5) {
        rechazar("se esperaban 5 columnas, llegaron " +
                 std::to_string(columnas.size()));
        continue;
      }

      Orden o;
      if (!parsearEntero(columnas[0], o.idOrden)) {
        rechazar("id_orden no es un entero valido");
        continue;
      }

      const std::string lado = columnas[1];
      if (lado == "compra")
        o.esCompra = true;
      else if (lado == "venta")
        o.esCompra = false;
      else {
        rechazar("lado '" + lado + "' invalido (de ser 'compra' o 'venta')");
        continue;
      }

      if (!parsearEntero(columnas[2], o.idNodo)) {
        rechazar("id_nodo no es un entero valido");
        continue;
      }
      if (!parsearDouble(columnas[3], o.kwh)) {
        rechazar("kwh no es un numero valido");
        continue;
      }
      if (!parsearDouble(columnas[4], o.precio)) {
        rechazar("precio no es un numero valido");
        continue;
      }

      o.secuencia = 0; // la asigna GridManager::insertarOrden()
      informe.filas.push_back(o);
    }

    return informe;
  }

};

// El PDF (seccion 4.1) llama a esta capa "CapaDatos". Se mantiene
// gestionDatos como nombre propio del proyecto y se agrega el alias del
// enunciado, para que el diagrama UML y el codigo hablen el mismo idioma.
using CapaDatos = gestionDatos;

#endif // GestionDatos_h
