// Pruebas del motor de subasta (GridManager) contra los casos de
// aceptación del PDF, sin base de datos.
//
// El enunciado incluye una sección "Casos de Uso de Ejemplo y Validación"
// con cuatro escenarios concretos y un ejemplo numérico. Cada prueba de
// acá reproduce uno de esos escenarios con el estado inicial que el
// enunciado supone y compara transacción por transacción y saldo por
// saldo. Si alguno se rompe, el motor ya no cumple el PDF.
//
// Uso:  make test          (o)  ./build/test_motor

#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "types.hpp"

namespace {

int fallos = 0;
int pruebas = 0;

bool cerca(double a, double b) { return std::fabs(a - b) < 1e-6; }

void check(const std::string &nombre, bool ok, const std::string &detalle = "") {
  ++pruebas;
  if (!ok)
    ++fallos;
  std::cout << (ok ? "  [ OK ]   " : "  [FALLA] ") << nombre;
  if (!detalle.empty())
    std::cout << "\n           -> " << detalle;
  std::cout << "\n";
}

TransaccionEnergia T(int vendedor, int comprador, double kwh, double precio) {
  return TransaccionEnergia(vendedor, comprador, kwh, precio, 0);
}

std::string fmt(const std::vector<TransaccionEnergia> &txns) {
  if (txns.empty())
    return "(ninguna)";
  std::string s;
  for (const auto &t : txns) {
    if (!s.empty())
      s += " | ";
    s += std::to_string(t.idVendedor) + "->" + std::to_string(t.idComprador) +
         " " + std::to_string(t.kwh) + "kWh@" + std::to_string(t.precio);
  }
  return s;
}

// Estado completo del dominio, como el que lleva main.cpp.
struct Escenario {
  std::map<int, double> saldos;
  std::vector<Orden> ordenes;
  int idBateria = -1;
  double cargaBateria = 0.0;
  double precioBase = 0.0;
  int tick = 0;
};

// Corre un tick con el MISMO orden que main.cpp, a través del método que
// el PDF le pide a GridManager (procesarTick). Si main.cpp y el motor se
// desincronizan, esta prueba lo detecta.
struct Resultado {
  std::vector<TransaccionEnergia> txns;
  std::map<int, double> saldos;
  double cargaBateria = 0.0;
  size_t descartadas = 0;
};

Resultado correr(const Escenario &e) {
  std::map<int, double> saldos = e.saldos;

  auto consultarSaldo = [&saldos](int id) -> double {
    auto it = saldos.find(id);
    return (it != saldos.end()) ? it->second : 0.0;
  };
  auto actualizarSaldo = [&saldos](int id, double nuevo) -> bool {
    auto it = saldos.find(id);
    if (it == saldos.end())
      return false;
    it->second = nuevo;
    return true;
  };
  auto nodoConocido = [&saldos](int id) { return saldos.count(id) > 0; };
  auto logger = [](const std::string &) {}; // silencio en las pruebas

  GridManager grid(consultarSaldo, actualizarSaldo, logger, nodoConocido);
  grid.setTickActual(e.tick);

  Resultado r;
  if (e.idBateria < 0) {
    r.txns = grid.procesarTick(e.ordenes, nullptr, e.precioBase);
    r.saldos = saldos;
    r.descartadas = grid.ordenesDescartadas();
    return r;
  }

  NodoAlmacenamiento bateria(e.idBateria, "Bateria", e.cargaBateria, 1000.0, 1e9);
  r.txns = grid.procesarTick(e.ordenes, &bateria, e.precioBase);
  r.saldos = saldos;
  r.descartadas = grid.ordenesDescartadas();

  // main.cpp descarga la batería recién después del COMMIT.
  double vendido = 0.0;
  for (const auto &t : r.txns)
    if (t.idVendedor == e.idBateria)
      vendido += t.kwh;
  bateria.liberarEnergia(vendido);
  r.cargaBateria = bateria.getCargaActual();
  return r;
}

void checkTxn(const std::string &caso, const Resultado &r,
              const std::vector<TransaccionEnergia> &esperadas) {
  bool ok = r.txns.size() == esperadas.size();
  for (size_t i = 0; ok && i < esperadas.size(); ++i) {
    const auto &a = r.txns[i];
    const auto &b = esperadas[i];
    ok = a.idVendedor == b.idVendedor && a.idComprador == b.idComprador &&
        cerca(a.kwh, b.kwh) && cerca(a.precio, b.precio);
  }
  check(caso + ": transacciones", ok,
        "esperadas [" + fmt(esperadas) + "]  obtenidas [" + fmt(r.txns) + "]");
}

void checkSaldo(const std::string &caso, const Resultado &r, int nodo,
                double esperado) {
  auto it = r.saldos.find(nodo);
  const double obtenido = (it == r.saldos.end()) ? 0.0 : it->second;
  check(caso + ": saldo nodo " + std::to_string(nodo) + " = " +
            std::to_string(esperado),
       cerca(obtenido, esperado), "obtenido " + std::to_string(obtenido));
}

} // namespace

int main() {
  std::cout << std::fixed << std::setprecision(4);
  std::cout << "Pruebas del motor de subasta contra los casos del PDF\n";

  // ------------------------------------------------------------------
  std::cout << "\n[5.3] Ejemplo numérico de la sección 5.3\n";
  {
    // 101 venta nodo 5, 10 kWh @2.5 | 201 compra nodo 7, 8 kWh @3.0
    // Espera: min(8,10)=8 kWh a (3.0+2.5)/2 = 2.75, y el nodo 202
    // (compra 9 kWh @2.4) no cruza porque 2.4 < 2.5.
    Escenario e;
    e.saldos = {{5, 1000}, {3, 1000}, {7, 1000}, {9, 1000}};
    e.ordenes = {{101, false, 5, 2.5, 10}, {102, false, 3, 5, 2.8},
                 {201, true, 7, 3.0, 8},   {202, true, 9, 2.4, 9}};
    e.tick = 3;
    checkTxn("5.3", correr(e), {T(5, 7, 8, 2.75)});
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Caso 1] Subasta básica exitosa (Tick 10:00)\n";
  {
    Escenario e;
    e.saldos = {{1, 100.0}, {2, 50.0}, {99, 1e9}};
    e.ordenes = {{1001, false, 2, 2.0, 5}, {2001, true, 1, 2.5, 5}};
    e.idBateria = 99;
    e.precioBase = 1.00;
    e.tick = 10;
    const auto r = correr(e);
    checkTxn("Caso 1", r, {T(2, 1, 5, 2.25)});
    checkSaldo("Caso 1", r, 1, 88.75);
    checkSaldo("Caso 1", r, 2, 61.25);
    check("Caso 1: la batería no interviene (carga 0)", cerca(r.cargaBateria, 0.0),
          "carga " + std::to_string(r.cargaBateria));
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Caso 2] Prioridad precio-tiempo y múltiples órdenes (Tick 12:00)\n";
  {
    Escenario e;
    e.saldos = {{10, 500.0}, {11, 200.0}, {20, 100.0},
                {21, 80.0},  {22, 50.0},  {99, 1e9}};
    e.ordenes = {{3001, false, 20, 2.0, 10}, {3002, false, 21, 2.0, 8},
                 {3003, false, 22, 2.5, 5},  {4001, true, 10, 3.0, 12},
                 {4002, true, 11, 2.2, 6}};
    e.idBateria = 99;
    e.precioBase = 1.50;
    e.tick = 12;
    const auto r = correr(e);
    checkTxn("Caso 2", r,
             {T(20, 10, 10, 2.5), T(21, 10, 2, 2.5), T(21, 11, 6, 2.1),
              T(22, 99, 5, 1.5)});
    checkSaldo("Caso 2", r, 10, 470.0);
    checkSaldo("Caso 2", r, 11, 187.4);
    checkSaldo("Caso 2", r, 20, 125.0);
    checkSaldo("Caso 2", r, 21, 97.6);
    checkSaldo("Caso 2", r, 22, 57.5);
    check("Caso 2: carga de batería = 5 kWh", cerca(r.cargaBateria, 5.0),
          "carga " + std::to_string(r.cargaBateria));
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Caso 3] Saldo insuficiente (Tick 18:00)\n";
  {
    // Nodo 30 tiene 15.00 y la compra cuesta 22.50: no puede pagar. El
    // motor en memoria lo detecta y el tick sigue sin abortar.
    Escenario e;
    e.saldos = {{30, 15.0}, {40, 60.0}, {99, 1e9}};
    e.ordenes = {{5001, false, 40, 2.0, 10}, {6001, true, 30, 2.5, 10}};
    e.idBateria = 99;
    e.precioBase = 2.00;
    e.tick = 18;
    const auto r = correr(e);
    bool sinCompraDel30 = true;
    for (const auto &t : r.txns)
      if (t.idComprador == 30)
        sinCompraDel30 = false;
    check("Caso 3: el nodo 30 no compra (saldo 15 < 22.50)", sinCompraDel30,
          "transacciones [" + fmt(r.txns) + "]");
    checkSaldo("Caso 3", r, 30, 15.0);
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Caso 4] Integración con batería (Ticks 14:00 y 15:00)\n";
  {
    Escenario e;
    e.saldos = {{50, 100.0}, {51, 200.0}, {99, 1e9}};
    e.ordenes = {{7001, false, 50, 2.0, 8}, {8001, true, 51, 1.8, 3}};
    e.idBateria = 99;
    e.cargaBateria = 0.0;
    e.precioBase = 1.00;
    e.tick = 14;
    const auto r14 = correr(e);
    checkTxn("Caso 4 tick 14", r14, {T(50, 99, 8, 1.0)});
    checkSaldo("Caso 4 tick 14", r14, 50, 108.0);
    check("Caso 4 tick 14: carga de batería = 8 kWh", cerca(r14.cargaBateria, 8.0),
          "carga " + std::to_string(r14.cargaBateria));

    Escenario e2;
    e2.saldos = r14.saldos;
    e2.ordenes = {{9001, true, 51, 1.5, 10}};
    e2.idBateria = 99;
    e2.cargaBateria = r14.cargaBateria;
    e2.precioBase = 1.20;
    e2.tick = 15;
    const auto r15 = correr(e2);
    checkTxn("Caso 4 tick 15", r15, {T(99, 51, 8, 1.35)});
    checkSaldo("Caso 4 tick 15", r15, 51, 189.2);
    check("Caso 4 tick 15: carga de batería = 0 kWh", cerca(r15.cargaBateria, 0.0),
          "carga " + std::to_string(r15.cargaBateria));
    checkSaldo("Caso 4 tick 15", r15, 50, 108.0);
  }

  // ------------------------------------------------------------------
  std::cout << "\n[Reglas de negocio] Validación de órdenes\n";
  {
    Escenario e; // kwh = 0
    e.saldos = {{1, 500.0}, {2, 500.0}};
    e.ordenes = {{1, false, 1, 2.0, 0}, {2, true, 2, 2.5, 5}};
    const auto r = correr(e);
    check("kWh = 0 se descarta y no genera transacción", r.txns.empty(),
          "transacciones [" + fmt(r.txns) + "]");
    check("kWh = 0 queda contabilizado como orden descartada", r.descartadas == 1,
          "descartadas " + std::to_string(r.descartadas));
  }
  {
    Escenario e; // kWh negativo
    e.saldos = {{1, 500.0}, {2, 500.0}};
    e.ordenes = {{1, false, 1, 2.0, -5}, {2, true, 2, 2.5, 5}};
    check("kWh negativo se descarta", correr(e).txns.empty());
  }
  {
    Escenario e; // precio <= 0
    e.saldos = {{1, 500.0}, {2, 500.0}};
    e.ordenes = {{1, false, 1, 0.0, 5}, {2, true, 2, 2.5, 5}};
    check("precio 0 se descarta (el PDF prohibe precios < 0)", correr(e).txns.empty());
    e.ordenes = {{1, false, 1, -2.0, 5}, {2, true, 2, 2.5, 5}};
    check("precio negativo se descarta", correr(e).txns.empty());
  }
  {
    Escenario e; // nodo inexistente
    e.saldos = {{1, 500.0}}; // el nodo 2 no existe
    e.ordenes = {{1, false, 1, 2.0, 5}, {2, true, 2, 2.5, 5}};
    const auto r = correr(e);
    check("orden de un nodo inexistente se descarta", r.txns.empty() && r.descartadas == 1,
          "transacciones [" + fmt(r.txns) + "], descartadas " +
              std::to_string(r.descartadas));
  }
  {
    Escenario e; // autoconsumo
    e.saldos = {{1, 500.0}};
    e.ordenes = {{1, false, 1, 2.0, 5}, {2, true, 1, 2.5, 5}};
    check("autoconsumo: un nodo no compra su propia energía", correr(e).txns.empty());
  }
  {
    Escenario e; // batería llena
    e.saldos = {{50, 500.0}, {51, 500.0}, {99, 1e9}};
    e.ordenes = {{7001, false, 50, 5.0, 10}, {8001, true, 51, 1.0, 1}};
    e.idBateria = 99;
    e.cargaBateria = 1000.0; // al 100% de su capacidad de 1000
    e.precioBase = 1.00;
    const auto r = correr(e);
    check("batería llena: no supera la capacidad máxima",
          r.cargaBateria <= 1000.0 + 1e-9,
          "carga " + std::to_string(r.cargaBateria));
  }
  {
    Escenario e; // orden de batería antes que el CSV (sección 3.3)
    // Batería con 10 kWh y tarifa 2.00; otro prosumidor vende al mismo
    // precio. El PDF exige que la oferta de la batería entre al libro
    // ANTES que las del CSV, así que despacha primero.
    e.saldos = {{50, 500.0}, {51, 500.0}, {60, 500.0}, {99, 1e9}};
    e.ordenes = {{7001, false, 50, 2.0, 10}, {8001, true, 60, 2.0, 10}};
    e.idBateria = 99;
    e.cargaBateria = 10.0;
    e.precioBase = 2.00;
    const auto r = correr(e);
    check("orden FIFO: la batería despacha antes que el CSV del mismo precio",
          !r.txns.empty() && r.txns[0].idVendedor == 99,
          "primera transacción [" + fmt(r.txns) + "]");
  }
  {
    Escenario e; // tick sin órdenes
    e.saldos = {{1, 500.0}, {99, 1e9}};
    e.idBateria = 99;
    e.precioBase = 1.00;
    const auto r = correr(e);
    check("tick sin órdenes no rompe nada", r.txns.empty() && cerca(r.cargaBateria, 0.0));
  }

  std::cout << "\n===============================================\n";
  std::cout << (fallos == 0 ? "TODAS LAS PRUEBAS PASARON" : "HAY FALLOS") << ": "
            << (pruebas - fallos) << "/" << pruebas << "\n";
  return fallos == 0 ? 0 : 1;
}
