#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include "types.hpp"
#include "GestionDatos.hpp"
#include "util/config.hpp"

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

int main(int argc, char *argv[]) {
  bool debug = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--debug") debug = true;
  }

  Config cfg = cargarConfig();

  gestionDatos gestor(cfg);
  gestor.setDebug(debug);

  gestor.crearTablas(cfg);

  auto datosNodos = gestor.cargarNodosDesdeBD();

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
  grid.setDebug(debug);

  // En modo debug imprime cada paso del flujo y espera Enter.
  auto debugPrint = [&debug](const std::string &msg) {
    if (!debug) return;
    std::cout << msg << std::endl;
    if (isatty(STDIN_FILENO)) {
      std::cout << "  (Enter para continuar)\n";
      std::cin.get();
    }
  };

  NodoAlmacenamiento *bateria = nullptr;
  for (const auto &[id, nodo] : nodos) {
    if (dynamic_cast<NodoAlmacenamiento *>(nodo)) {
      bateria = static_cast<NodoAlmacenamiento *>(nodo);
      break;
    }
  }

  size_t totalTransacciones = 0;
  double totalKwh = 0.0;
  size_t totalLecturas = 0;

  for (int hora = 0; hora < 24; ++hora) {
    grid.setTickActual(hora);

    std::string sufijo = (hora < 10) ? "0" : "";
    std::string horaStr = sufijo + std::to_string(hora);
    std::string ruta = cfg.datosDir + "/ofertas_" + horaStr + ".csv";

    debugPrint("[Tick " + horaStr + "] Leyendo " + ruta);

    auto ordenes = gestor.leerCSV(ruta);
    int compras = 0, ventas = 0;
    for (const auto &o : ordenes) {
      grid.insertarOrden(o);
      if (o.esCompra) ++compras;
      else ++ventas;
    }
    debugPrint("[Tick " + horaStr + "] Órdenes cargadas: " +
               std::to_string(ordenes.size()) + " (" +
               std::to_string(compras) + " compra / " +
               std::to_string(ventas) + " venta)");

    if (bateria) {
      double tarifa = gestor.leerTarifa(hora);
      debugPrint("[Tick " + horaStr + "] Tarifa hora = " +
                 std::to_string(tarifa) + " | Batería nodo " +
                 std::to_string(bateria->getId()) + " carga = " +
                 std::to_string(bateria->getCargaActual()) + " kWh");
      grid.insertarOfertaBateria(bateria->getId(), bateria->getCargaActual(),
                                 tarifa);
      grid.ejecutarMatching();
      grid.transferirExcedentesABateria(*bateria, tarifa);
    } else {
      grid.ejecutarMatching();
    }
    grid.reevaluarPendientesPorSaldo();

    const auto &txns = grid.getTransacciones();
    double kwhTick = 0.0;
    for (const auto &t : txns) kwhTick += t.kwh;
    debugPrint("[Tick " + horaStr + "] Matching: " +
               std::to_string(txns.size()) + " transacciones (" +
               std::to_string(kwhTick) + " kWh)");

    // Descargar la batería por la energía vendida en el tick
    // (matching + reintentos por saldo).
    if (bateria) {
      double vendido = 0.0;
      for (const auto &t : txns) {
        if (t.idVendedor == bateria->getId()) vendido += t.kwh;
      }
      bateria->liberarEnergia(vendido);
      debugPrint("[Tick " + horaStr + "] Batería descargada: " +
                 std::to_string(vendido) + " kWh (carga restante " +
                 std::to_string(bateria->getCargaActual()) + " kWh)");
    }

    // Persistencia transaccional del tick (bloque atómico)
    if (gestor.persistirTransacciones(txns)) {
      totalTransacciones += txns.size();
      totalKwh += kwhTick;
      debugPrint("[Tick " + horaStr + "] COMMIT OK: " +
                 std::to_string(txns.size()) + " transacciones persistidas");

      for (const auto &[id, nodo] : nodos) {
        gestor.actualizarSaldo(id, nodo->getSaldoCuenta());
      }
      debugPrint("[Tick " + horaStr + "] Saldos actualizados (" +
                 std::to_string(nodos.size()) + " nodos)");

      // Lecturas históricas por nodo participante del tick
      std::map<int, std::pair<double, double>> lecturas;
      for (const auto &t : txns) {
        lecturas[t.idVendedor].first += t.kwh;   // producción
        lecturas[t.idComprador].second += t.kwh; // consumo
      }
      for (const auto &[idNodo, prodCons] : lecturas) {
        gestor.insertarLectura(idNodo, hora, prodCons.first, prodCons.second);
      }
      totalLecturas += lecturas.size();
      debugPrint("[Tick " + horaStr + "] Lecturas históricas: " +
                 std::to_string(lecturas.size()) + " registros");
    } else {
      logger("[Tick " + horaStr +
             "] Transacciones rechazadas: ROLLBACK, saldos sin cambios.");
    }

    grid.limpiarLibroAlFinalDelTick();
  }

  for (auto &[id, nodo] : nodos) {
    delete nodo;
  }
  nodos.clear();

  debugPrint("[Fin] Resumen: " + std::to_string(totalTransacciones) +
             " transacciones (" + std::to_string(totalKwh) + " kWh), " +
             std::to_string(totalLecturas) + " lecturas históricas.");

  std::cout << "Simulación finalizada.\n";
  return 0;
}