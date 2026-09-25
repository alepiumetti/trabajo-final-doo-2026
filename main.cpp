#include <fstream>
#include <iostream>
#include <map>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "types.hpp"

#include "GestionDatos.hpp"

static NodoRed *construirNodo(const DatoNodo &d) {
  if (d.tipo == "Prosumidor") {
    return new NodoProsumidor(d.id, d.ubicacion, 0.0, 0.0, d.saldo);
  }
  if (d.tipo == "Bateria") {
    return new NodoAlmacenamiento(d.id, d.ubicacion, 0.0, 1000.0, d.saldo);
  }
  PerfilConsumo perfil = Residencial;
  if (d.perfil == "Comercial") perfil = Comercial;
  else if (d.perfil == "Industrial") perfil = Industrial;
  return new NodoConsumidor(d.id, d.ubicacion, perfil, 0.0, d.saldo);
}

int main() {
  gestionDatos gestor;

  gestor.crearTablas();

  auto datosNodos = gestor.cargarNodos("./datos/nodos.csv");

  std::map<int, NodoRed *> nodos;
  for (const auto &d : datosNodos) {
    nodos[d.id] = construirNodo(d);
  }

  auto consultarSaldo = [&nodos](int id) -> double {
    auto it = nodos.find(id);
    return (it != nodos.end()) ? it->second->getSaldoCuenta() : 0.0;
  };
  auto actualizarSaldo = [&nodos](int id, double nuevoSaldo) -> bool {
    auto it = nodos.find(id);
    if (it == nodos.end()) return false;
    it->second->setSaldoCuenta(nuevoSaldo);
    return true;
  };
  auto logger = [](const std::string &msg) { std::cout << msg << std::endl; };

  GridManager grid(consultarSaldo, actualizarSaldo, logger);

  NodoAlmacenamiento *bateria = nullptr;
  for (const auto &[id, nodo] : nodos) {
    if (dynamic_cast<NodoAlmacenamiento *>(nodo)) {
      bateria = static_cast<NodoAlmacenamiento *>(nodo);
      break;
    }
  }

  for (int hora = 0; hora < 24; ++hora) {
    grid.setTickActual(hora);

    std::string sufijo = (hora < 10) ? "0" : "";
    std::string ruta =
        "./datos/ofertas_" + sufijo + std::to_string(hora) + ".csv";
    auto ordenes = gestor.leerCSV(ruta);
    for (const auto &o : ordenes) {
      grid.insertarOrden(o);
    }

    if (bateria) {
      double tarifa = gestor.leerTarifa(hora);
      grid.insertarOfertaBateria(bateria->getId(), bateria->getCargaActual(),
                                 tarifa);
      grid.ejecutarMatching();
      grid.transferirExcedentesABateria(*bateria, tarifa);
    } else {
      grid.ejecutarMatching();
    }
    grid.reevaluarPendientesPorSaldo();

    // Descargar la batería por la energía vendida en el tick
    // (matching + reintentos por saldo).
    if (bateria) {
      double vendido = 0.0;
      for (const auto &t : grid.getTransacciones()) {
        if (t.idVendedor == bateria->getId()) vendido += t.kwh;
      }
      bateria->liberarEnergia(vendido);
    }

    for (const auto &t : grid.getTransacciones()) {
      gestor.insertarTransaccion(t);
    }

    for (const auto &[id, nodo] : nodos) {
      gestor.actualizarSaldo(id, nodo->getSaldoCuenta());
    }

    grid.limpiarLibroAlFinalDelTick();
  }

  for (auto &[id, nodo] : nodos) {
    delete nodo;
  }
  nodos.clear();

  std::cout << "Simulación finalizada.\n";
  return 0;
}
