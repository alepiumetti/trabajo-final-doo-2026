#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "GestionDatos.hpp"
#include "types.hpp"
#include "util/config.hpp"

namespace {

// Traduce el 'tipo' de la tabla NODOS a la subclase concreta de NodoRed.
// Se resuelve con unique_ptr: el mapa de nodos es dueño de todos
// los objetos y no hay delete manual.
std::unique_ptr<NodoRed> construirNodo(const DatoNodo &d) {
  if (d.tipo == "Prosumidor") {
    return std::make_unique<NodoProsumidor>(d.id, d.ubicacion, 0.0, 0.0,
                                            d.saldo);
  }
  if (d.tipo == "Bateria") {
    return std::make_unique<NodoAlmacenamiento>(d.id, d.ubicacion, 0.0, 1000.0,
                                                d.saldo);
  }
  PerfilConsumo perfil = Residencial;
  if (d.perfil == "Comercial")
    perfil = Comercial;
  else if (d.perfil == "Industrial")
    perfil = Industrial;
  return std::make_unique<NodoConsumidor>(d.id, d.ubicacion, perfil, 0.0,
                                          d.saldo);
}

} // namespace

int main(int argc, char *argv[]) {
  bool debug = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--debug")
      debug = true;
  }

  Config cfg = cargarConfig();

  // Corrida limpia y determinista: cada ejecución arranca desde la semilla.
  std::remove(cfg.dbPath.c_str());

  try {
    GestionDatos gestor(cfg);
    gestor.setDebug(debug);
    gestor.crearTablas(cfg);

    auto datosNodos = gestor.cargarNodosDesdeBD();
    if (datosNodos.empty()) {
      std::cerr << "La tabla NODOS quedó vacía: no hay nada que simular.\n";
      return 1;
    }

    // unique_ptr: el mapa es dueño de los nodos y los libera solo.
    std::map<int, std::unique_ptr<NodoRed>> nodos;
    for (const auto &d : datosNodos)
      nodos.emplace(d.id, construirNodo(d));

    auto consultarSaldo = [&nodos](int id) -> double {
      auto it = nodos.find(id);
      return (it != nodos.end()) ? it->second->getSaldoCuenta() : 0.0;
    };
    auto actualizarSaldo = [&nodos](int id, double nuevoSaldo) -> bool {
      auto it = nodos.find(id);
      if (it == nodos.end())
        return false;
      it->second->setSaldoCuenta(nuevoSaldo);
      return true;
    };
    auto logger = [](const std::string &msg) { std::cout << msg << std::endl; };

    GridManager grid(consultarSaldo, actualizarSaldo, logger);
    grid.setDebug(debug);

    // En modo debug imprime cada paso del flujo y espera Enter.
    auto debugPrint = [&debug](const std::string &msg) {
      if (!debug)
        return;
      std::cout << msg << std::endl;
      if (isatty(STDIN_FILENO)) {
        std::cout << "  (Enter para continuar)\n";
        std::cin.get();
      }
    };

    NodoAlmacenamiento *bateria = nullptr;
    for (const auto &[id, nodo] : nodos) {
      if (dynamic_cast<NodoAlmacenamiento *>(nodo.get())) {
        bateria = static_cast<NodoAlmacenamiento *>(nodo.get());
        break;
      }
    }

    size_t totalTransacciones = 0;
    double totalKwh = 0.0;
    size_t totalLecturas = 0;

    for (int hora = 0; hora < 24; ++hora) {
      std::cout << " \n ========== Tick " << hora << " ========== \n"
                << std::endl;
      grid.setTickActual(hora);

      std::string sufijo = (hora < 10) ? "0" : "";
      const std::string horaStr = sufijo + std::to_string(hora);
      const std::string ruta = cfg.datosDir + "/ofertas_" + horaStr + ".csv";
      const std::string tickHora = timestampTick(cfg, hora);

      if (bateria) {
        std::cout << "[Tick " << horaStr << "] Batería inicio: nodo "
                  << bateria->getId() << ", " << bateria->estado() << std::endl;
      }

      debugPrint("[Tick " + horaStr + "] Leyendo " + ruta);

      const auto ordenes = GestionDatos::leerCSV(ruta);
      int compras = 0, ventas = 0;
      for (const auto &o : ordenes) {
        if (o.esCompra)
          ++compras;
        else
          ++ventas;
      }
      debugPrint("[Tick " + horaStr +
                 "] Órdenes cargadas: " + std::to_string(ordenes.size()) +
                 " (" + std::to_string(compras) + " compra / " +
                 std::to_string(ventas) + " venta)");

      const double tarifa = gestor.leerTarifa(hora);
      if (bateria) {
        debugPrint(
            "[Tick " + horaStr + "] Tarifa hora = " + std::to_string(tarifa) +
            " | Batería nodo " + std::to_string(bateria->getId()) +
            " carga = " + std::to_string(bateria->getCargaActual()) + " kWh");
      }

      // Orquestración del tick (oferta de batería -> CSV -> matching ->
      // excedentes a batería) dentro de GridManager::procesarTick.
      const std::vector<TransaccionEnergia> txns =
          grid.procesarTick(ordenes, bateria, tarifa);

      double kwhTick = 0.0;
      for (const auto &t : txns)
        kwhTick += t.kwh;
      debugPrint("[Tick " + horaStr +
                 "] Matching: " + std::to_string(txns.size()) +
                 " transacciones (" + std::to_string(kwhTick) + " kWh)");

      // Persistencia transaccional del tick (bloque atómico): transacciones,
      // saldos y lecturas se confirman juntos o no se confirma nada.
      const ResultadoTick r = gestor.persistirTick(txns, tickHora);

      if (r.ok) {
        totalTransacciones += r.transacciones;
        totalKwh += kwhTick;
        totalLecturas += r.lecturas;
        debugPrint("[Tick " + horaStr + "] COMMIT OK: " +
                   std::to_string(r.transacciones) + " transacciones y " +
                   std::to_string(r.lecturas) + " lecturas persistidas");

        // La batería se descarga recién con el tick confirmado.
        if (bateria) {
          double vendido = 0.0;
          for (const auto &t : txns) {
            if (t.idVendedor == bateria->getId())
              vendido += t.kwh;
          }
          bateria->liberarEnergia(vendido);
          debugPrint("[Tick " + horaStr + "] Batería descargada: " +
                     std::to_string(vendido) + " kWh (carga restante " +
                     std::to_string(bateria->getCargaActual()) + " kWh)");
        }
      } else {
        logger("[Tick " + horaStr + "] Tick rechazado: " + r.motivo);
      }

      if (bateria) {
        std::cout << "[Tick " << horaStr << "] Batería fin: nodo "
                  << bateria->getId() << ", " << bateria->estado() << std::endl;
      }

      grid.limpiarLibroAlFinalDelTick();
    }

    // El mapa de unique_ptr se destruye solo: no hace falta delete.
    nodos.clear();

    std::cout << "\n \n ========== Resumen ========== \n";
    std::cout << "Transacciones persistidas: " << totalTransacciones << "\n";
    std::cout << "Energía transada: " << totalKwh << " kWh\n";
    std::cout << "Lecturas históricas: " << totalLecturas << "\n";
    std::cout << "Simulación finalizada.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Error fatal: " << e.what() << "\n";
    return 1;
  }
}
