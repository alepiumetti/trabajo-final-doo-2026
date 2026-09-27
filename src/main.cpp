#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ecogrid.h"

namespace {

// Traduce el 'tipo' de la tabla NODOS a la subclase concreta de NodoRed.
// Se resuelve con unique_ptr: el mapa de nodos es dueño de todos
// los objetos y no hay delete manual.
std::unique_ptr<NodoRed> construirNodo(const DatoNodo &d) {
  if (d.tipo == TIPO_PROSUMIDOR) {
    return std::make_unique<NodoProsumidor>(d.id, d.ubicacion, 0.0, 0.0,
                                            d.saldo);
  }
  if (d.tipo == TIPO_BATERIA) {
    return std::make_unique<NodoAlmacenamiento>(d.id, d.ubicacion, 0.0,
                                                CAPACIDAD_BATERIA, d.saldo);
  }
  PerfilConsumo perfil = Residencial;
  if (d.perfil == PERFIL_COMERCIAL)
    perfil = Comercial;
  else if (d.perfil == PERFIL_INDUSTRIAL)
    perfil = Industrial;
  return std::make_unique<NodoConsumidor>(d.id, d.ubicacion, perfil, 0.0,
                                          d.saldo);
}

} // namespace

int main(int argc, char *argv[]) {
  bool debug = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == FLAG_DEBUG)
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

    // Estructura <id, NodoRed> para consulatr y actualizar saldos.
    std::map<int, std::unique_ptr<NodoRed>> nodos;

    for (const auto &d : datosNodos)
      nodos.emplace(d.id, construirNodo(d));

    // Función lambda -> recibe int idNodo como id y devuelve saldoCuenta
    auto consultarSaldo = [&nodos](int id) -> double {
      auto nodoEncontrado = nodos.find(id);
      return (nodoEncontrado != nodos.end())
                 ? nodoEncontrado->second->getSaldoCuenta()
                 : 0.0;
    };
    auto actualizarSaldo = [&nodos](int id, double nuevoSaldo) -> bool {
      auto nodoEncontrado = nodos.find(id);
      if (nodoEncontrado == nodos.end())
        return false;
      nodoEncontrado->second->setSaldoCuenta(nuevoSaldo);
      return true;
    };

    auto logger = [](const std::string &msg) -> void {
      std::cout << msg << std::endl;
    };

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

    // Buscamos la bateria
    NodoAlmacenamiento *bateria = nullptr;
    for (const auto &par : nodos) {
      if (par.second->esBateria()) {
        bateria = static_cast<NodoAlmacenamiento *>(par.second.get());
        break;
      }
    }

    size_t totalTransacciones = 0;
    double totalKwh = 0.0;
    size_t totalLecturas = 0;

    for (int hora = 0; hora < HORAS_POR_DIA; ++hora) {
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
      for (const auto &orden : ordenes) {
        if (orden.esCompra)
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
      const std::vector<TransaccionEnergia> transacciones =
          grid.procesarTick(ordenes, bateria, tarifa);

      double kwhTick = 0.0;
      for (const auto &transaccion : transacciones) {
        kwhTick += transaccion.kwh;
      }
      debugPrint("[Tick " + horaStr +
                 "] Matching: " + std::to_string(transacciones.size()) +
                 " transacciones (" + std::to_string(kwhTick) + " kWh)");

      // Persistencia transaccional del tick (bloque atómico): transacciones,
      // saldos y lecturas se confirman juntos o no se confirma nada.
      const ResultadoTick resultado =
          gestor.persistirTick(transacciones, tickHora);

      if (resultado.ok) { // Si el tick se confirmó
        totalTransacciones += resultado.transacciones;
        totalKwh += kwhTick;
        totalLecturas += resultado.lecturas;
        debugPrint("[Tick " + horaStr +
                   "] COMMIT OK: " + std::to_string(resultado.transacciones) +
                   " transacciones y " + std::to_string(resultado.lecturas) +
                   " lecturas persistidas");

        // La batería se descarga lo vendido recién con el tick confirmado
        // evistando inconsistencia si el commit falla.
        if (bateria) {
          double vendido = 0.0;
          for (const auto &transaccion : transacciones) {
            if (transaccion.idVendedor == bateria->getId())
              vendido += transaccion.kwh;
          }
          bateria->liberarEnergia(vendido);
          debugPrint("[Tick " + horaStr + "] Batería descargada: " +
                     std::to_string(vendido) + " kWh (carga restante " +
                     std::to_string(bateria->getCargaActual()) + " kWh)");
        }
      } else {
        logger("[Tick " + horaStr + "] Tick rechazado: " + resultado.motivo);
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
