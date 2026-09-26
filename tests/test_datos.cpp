// Pruebas de la capa de datos (CapaDatos) contra el esquema real de
// sql/crear_esquema.sql. Usa una base temporal aparte: no toca ejemplo.db.
//
// Cubre el trigger de saldo insuficiente, las restricciones CHECK / FK, la
// atomicidad por tick, el procedimiento almacenado
// actualizar_saldo_y_lecturas y la coherencia entre el ledger del tick y
// los saldos.
//
// Uso:  make test          (o)  ./build/test_datos

#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "GestionDatos.hpp"
#include "types.hpp"

namespace {

int fallos = 0;
int pruebas = 0;

void check(const std::string &nombre, bool ok, const std::string &detalle = "") {
  ++pruebas;
  if (!ok)
    ++fallos;
  std::cout << (ok ? "  [ OK ]   " : "  [FALLA] ") << nombre;
  if (!detalle.empty())
    std::cout << "\n           -> " << detalle;
  std::cout << "\n";
}

bool cerca(double a, double b) { return std::fabs(a - b) < 1e-6; }

// El mapa COMPLETO de saldos iniciales de la semilla. persistirTick
// verifica todos los nodos de NODOS, así que hay que pasarlo entero (es lo
// que hace main.cpp).
std::map<int, double> saldosSemilla(std::map<int, double> overrides = {}) {
  std::map<int, double> m{{1, 100},     {2, 50},   {3, 100},   {4, 150},
                          {6, 100},     {10, 500}, {11, 200},  {20, 100},
                          {21, 80},     {22, 50},  {30, 15},   {40, 60},
                          {50, 100},    {51, 200}, {99, 1000000}};
  for (const auto &kv : overrides)
    m[kv.first] = kv.second;
  return m;
}

} // namespace

int main() {
  std::cout << std::fixed << std::setprecision(4);
  std::cout << "Pruebas de la capa de datos\n";

  const std::string dbTemporal = "test_datos.db";
  std::remove(dbTemporal.c_str());

  Config cfg;
  cfg.dbPath = dbTemporal;
  cfg.sqlPath = "./sql/crear_esquema.sql";
  cfg.datosDir = "./datos";
  cfg.fechaSimulada = "2026-06-01";

  CapaDatos g(cfg);
  g.crearTablas(cfg);

  const std::string tick10 = timestampTick(cfg, 10);
  const std::string tick18 = timestampTick(cfg, 18);

  // ------------------------------------------------------------------
  std::cout << "\n[Trigger] trg_validar_saldo — Caso 3 del PDF\n";
  {
    // Nodo 30 tiene 15.00 y la compra cuesta 10 * 2.25 = 22.50.
    g.iniciarTransaccion();
    const bool ok = g.insertarTransaccion(TransaccionEnergia(40, 30, 10.0, 2.25, 18));
    check("el trigger rechaza la compra con saldo insuficiente", !ok,
          ok ? "la inserción pasó y debería haber fallado" : "");
    g.abortarTransaccion();
  }
  {
    const std::vector<TransaccionEnergia> txns{
        TransaccionEnergia(40, 30, 10.0, 2.25, 18)};
    const auto r = g.persistirTick(txns, saldosSemilla({{30, 15.0}, {40, 60.0}}),
                                   tick18);
    check("persistirTick devuelve ok=false ante saldo insuficiente", !r.ok,
          r.motivo);
    check("el motivo identifica al trigger", r.motivo.find("Trigger") != std::string::npos,
          r.motivo);
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Restricciones] CHECK y FK\n";
  {
    g.iniciarTransaccion();
    const bool ok = g.insertarTransaccion(TransaccionEnergia(40, 10, 0.0, 2.25, 18));
    check("kwh = 0 rechazado por CHECK (kwh > 0)", !ok);
    g.abortarTransaccion();
  }
  {
    g.iniciarTransaccion();
    const bool ok = g.insertarTransaccion(TransaccionEnergia(40, 10, 5.0, 0.0, 18));
    check("precio = 0 rechazado por CHECK (precio_unitario > 0)", !ok);
    g.abortarTransaccion();
  }
  {
    g.iniciarTransaccion();
    const bool ok = g.insertarTransaccion(TransaccionEnergia(40, 10, 5.0, -1.0, 18));
    check("precio negativo rechazado por CHECK", !ok);
    g.abortarTransaccion();
  }
  {
    g.iniciarTransaccion();
    const bool ok = g.insertarTransaccion(TransaccionEnergia(40, 7777, 5.0, 2.0, 18));
    check("nodo inexistente rechazado por Foreign Key", !ok);
    g.abortarTransaccion();
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Atomicidad] Un tick se confirma entero o no se confirma\n";
  {
    // Dos transacciones: la primera es válida, la segunda.no. El bloque
    // tiene que revertirse completo: si la primera quedara, la atomicidad
    // estaría rota.
    const std::vector<TransaccionEnergia> txns{
        TransaccionEnergia(2, 1, 5.0, 2.25, 10),   // válida
        TransaccionEnergia(40, 7777, 1.0, 2.0, 10) // FK inválida
    };
    const auto r = g.persistirTick(txns, saldosSemilla({{1, 88.75}, {2, 61.25}}),
                                   tick10);
    check("persistirTick devuelve ok=false con una transacción inválida", !r.ok,
          r.motivo);

    // El nodo 1 debería conservar su saldo de semilla: la primera
    // transacción se revirtió con el bloque.
    const auto nodos = g.cargarNodosDesdeBD();
    double saldo1 = -1.0;
    for (const auto &n : nodos)
      if (n.id == 1)
        saldo1 = n.saldo;
    check("la transacción válida del mismo tick también se revirtió",
          cerca(saldo1, 100.0), "saldo del nodo 1 = " + std::to_string(saldo1));
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Caso 1 del PDF] Un tick que sí se persiste\n";
  {
    const std::vector<TransaccionEnergia> txns{
        TransaccionEnergia(2, 1, 5.0, 2.25, 10)};
    const auto r = g.persistirTick(txns, saldosSemilla({{1, 88.75}, {2, 61.25}}),
                                   tick10);
    check("persistirTick confirma el tick", r.ok, r.motivo);
    check("el tick escribe 1 transacción y 2 lecturas",
          r.transacciones == 1 && r.lecturas == 2,
          "transacciones=" + std::to_string(r.transacciones) +
              " lecturas=" + std::to_string(r.lecturas));

    const auto nodos = g.cargarNodosDesdeBD();
    double s1 = 0.0, s2 = 0.0;
    for (const auto &n : nodos) {
      if (n.id == 1) s1 = n.saldo;
      if (n.id == 2) s2 = n.saldo;
    }
    check("saldo del nodo 1 = 88.75 en la BD", cerca(s1, 88.75),
          std::to_string(s1));
    check("saldo del nodo 2 = 61.25 en la BD", cerca(s2, 61.25),
          std::to_string(s2));
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Procedimiento almacenado] actualizar_saldo_y_lecturas\n";
  {
    // Firma exacta del PDF: (p_id_nodo, p_kwh, p_precio, p_tipo_operacion).
    g.iniciarTransaccion();
    const bool ok = g.invocarActualizarSaldoYLecturas(1, 2.0, 1.0, "compra");
    check("la operación 'compra' se ejecuta", ok);
    g.abortarTransaccion();

    g.iniciarTransaccion();
    const bool mala =
        g.invocarActualizarSaldoYLecturas(1, 2.0, 1.0, "sideways");
    check("un tipo_operacion inválido se rechaza", !mala);
    g.abortarTransaccion();

    g.iniciarTransaccion();
    const bool fantasma =
        g.invocarActualizarSaldoYLecturas(4242, 2.0, 1.0, "venta");
    check("un nodo inexistente se rechaza", !fantasma);
    g.abortarTransaccion();
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Coherencia] El ledger del tick debe explicar los saldos\n";
  {
    const std::vector<TransaccionEnergia> txns{
        TransaccionEnergia(2, 1, 5.0, 2.25, 11)};
    // El modelo en memoria dice 999: no cuadra con el ledger.
    const auto r = g.persistirTick(txns, saldosSemilla({{1, 999.0}, {2, 61.25}}),
                                   timestampTick(cfg, 11));
    check("persistirTick detecta memoria desincronizada y revierte", !r.ok,
          r.motivo);
    check("el motivo menciona la incoherencia",
          r.motivo.find("memoria") != std::string::npos, r.motivo);
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Configuración] CONFIG_TARIFAS contra los valores del PDF\n";
  {
    struct { int hora; double pdf; } casos[] = {
        {10, 1.00}, {12, 1.50}, {14, 1.00}, {15, 1.20}, {18, 2.00}};
    bool todosOk = true;
    std::string detalle;
    for (const auto &c : casos) {
      const double v = g.leerTarifa(c.hora);
      if (!cerca(v, c.pdf)) {
        todosOk = false;
        detalle += "hora " + std::to_string(c.hora) + ": PDF=" +
                   std::to_string(c.pdf) + " esquema=" + std::to_string(v) + "; ";
      }
    }
    check("las 5 tarifas de los casos del PDF coinciden con el esquema", todosOk,
          detalle);
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Semilla] Nodos del PDF\n";
  {
    const auto nodos = g.cargarNodosDesdeBD();
    bool hay99 = false, bateriaOk = false;
    for (const auto &n : nodos) {
      if (n.id == 99)
        hay99 = true;
      if (n.tipo == "Bateria") {
        bateriaOk = (n.id == BATERIA);
      }
    }
    check("existe el nodo 99 que usa el PDF como batería", hay99);
    check("el nodo Batería de la BD coincide con BATERIA de types.hpp",
          bateriaOk, "BATERIA = " + std::to_string(BATERIA));
  }

  // ------------------------------------------------------------------
  std::cout << "\n[CSV] Lectura tolerante a datos inválidos\n";
  {
    struct Caso { const char *contenido; const char *descripcion; };
    // Se escriben archivos temporales y se leen con leerCSVConInforme.
    const std::string rutas[] = {"/tmp/ecogrid_t1.csv", "/tmp/ecogrid_t2.csv",
                                "/tmp/ecogrid_t3.csv", "/tmp/ecogrid_t4.csv"};
    const char *contenidos[] = {
        "id_orden,lado,id_nodo,kwh,precio\n1,venta,2,abc,2.0\n",
        "id_orden,lado,id_nodo,kwh,precio\n1,tal,2,5,2.0\n",
        "id_orden,lado,id_nodo,kwh,precio\n1,venta,2,5\n",
        "id_orden,lado,id_nodo,kwh,precio\n1,venta,2,5kg,2.0\n"};
    const char *descs[] = {"kWh no numérico", "lado inválido",
                           "faltan columnas", "kWh con sufijo"};
    for (int i = 0; i < 4; ++i) {
      FILE *f = std::fopen(rutas[i].c_str(), "w");
      std::fputs(contenidos[i], f);
      std::fclose(f);
      const auto inf = CapaDatos::leerCSVConInforme(rutas[i]);
      check(std::string("CSV inválido (") + descs[i] + ") se rechaza sin romper",
            inf.filas.empty() && !inf.rechazadas.empty(),
            inf.rechazadas.empty() ? "no se reportó nada" : inf.rechazadas[0]);
    }
    // Un archivo que no existe no debe lanzar.
    const auto inexistente = CapaDatos::leerCSVConInforme("/tmp/ecogrid_no_existe.csv");
    check("un CSV inexistente se reporta sin lanzar excepción",
          inexistente.filas.empty() && !inexistente.rechazadas.empty());

    // Y un CSV válido se lee bien.
    FILE *f = std::fopen("/tmp/ecogrid_ok.csv", "w");
    std::fputs("id_orden,lado,id_nodo,kwh,precio\n"
               "1001,venta,2,5,2.0\n"
               "2001,compra,1,5,2.5\n", f);
    std::fclose(f);
    const auto ok = CapaDatos::leerCSVConInforme("/tmp/ecogrid_ok.csv");
    check("un CSV válido se lee completo",
          ok.filas.size() == 2 && ok.rechazadas.empty() && ok.filas[1].esCompra,
          "filas=" + std::to_string(ok.filas.size()));
  }

  std::remove(dbTemporal.c_str());

  std::cout << "\n===============================================\n";
  std::cout << (fallos == 0 ? "TODAS LAS PRUEBAS PASARON" : "HAY FALLOS") << ": "
            << (pruebas - fallos) << "/" << pruebas << "\n";
  return fallos == 0 ? 0 : 1;
}
