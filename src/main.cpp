// EcoGrid - punto de entrada de la simulacion.
//
// Recorre los 24 ticks del dia. Cada tick:
//   1. Carga las ordenes de datos/ofertas_HH.csv.
//   2. GridManager::procesarTick corre la subasta de doble continua.
//   3. CapaDatos::persistirTick escribe el tick entero en un unico bloque
//      transaccional (BEGIN IMMEDIATE ... COMMIT) junto con el movimiento
//      de saldos y las lecturas historicas.
//   4. Solo si el COMMIT es exitoso, la bateria se descarga.
//
// La orquestracion del tick vive en GridManager (ver types.hpp); este
// archivo se limita a cablear las capas y a manejar el ciclo del dia.

#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
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
    return std::make_unique<NodoProsumidor>(d.id, d.ubicacion, 0.0, 0.0, d.saldo);
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
  return std::make_unique<NodoConsumidor>(d.id, d.ubicacion, perfil, 0.0, d.saldo);
}

void imprimirUso(const char *programa) {
  std::cout
      << "Uso: " << programa << " [opciones]\n"
      << "\n"
      << "  --reset            Borra la base anterior y corre los 24 ticks\n"
      << "                     desde el estado inicial de la semilla.\n"
      << "  --debug            Imprime cada paso y espera Enter para\n"
      << "                     continuar (solo interactivo).\n"
      << "  --trigger-prueba   Instala un trigger extra que rechaza toda\n"
      << "                     compra individual de mas de 30 creditos,\n"
      << "                     para poder ver un ROLLBACK de punta a\n"
      << "                     punta. Ver README.\n"
      << "  -h, --help         Muestra esta ayuda.\n"
      << "\n"
      << "Sin opciones corre la simulacion sobre la base existente\n"
      << "(las corridas se acumulan; use --reset para empezar de cero).\n";
}

} // namespace

int main(int argc, char *argv[]) {
  bool debug = false;
  bool reset = false;
  bool triggerPrueba = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--debug")
      debug = true;
    else if (arg == "--reset")
      reset = true;
    else if (arg == "--trigger-prueba")
      triggerPrueba = true;
    else if (arg == "-h" || arg == "--help") {
      imprimirUso(argv[0]);
      return 0;
    } else {
      std::cerr << "Opcion desconocida: " << arg << "\n\n";
      imprimirUso(argv[0]);
      return 2;
    }
  }

  Config cfg = cargarConfig();

  // --reset: arrancar de una base limpia. Sin esto las corridas se acumulan
  // (TRANSACCIONES solo crece) y es fácil mirar filas de una corrida
  // anterior y creer que un tick revertido quedó persistido.
  if (reset) {
    if (std::remove(cfg.dbPath.c_str()) == 0)
      std::cout << "Base anterior eliminada (" << cfg.dbPath << ").\n";
    else
      std::cout << "No había base anterior que eliminar (" << cfg.dbPath
                << ").\n";
  }

  try {
    CapaDatos gestor(cfg);
    gestor.setDebug(debug);
    gestor.crearTablas(cfg);

    // Límite del trigger de prueba, en créditos por compra individual.
    constexpr double kLimiteTriggerPrueba = 30.0;
    if (triggerPrueba)
      gestor.activarTriggerPrueba(kLimiteTriggerPrueba);
    else
      gestor.desactivarTriggerPrueba();

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
    // El PDF (sección 6.1) exige que los nodos existan en la BD. Esta
    // comprobación evita que un id inexistente en el CSV entre al libro y
    // sea descartado después con el diagnóstico equivocado ("saldo 0").
    auto nodoConocido = [&nodos](int id) { return nodos.count(id) > 0; };
    auto logger = [](const std::string &msg) { std::cout << msg << std::endl; };

    GridManager grid(consultarSaldo, actualizarSaldo, logger, nodoConocido);
    grid.setDebug(debug);

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
    size_t totalRechazados = 0;
    size_t ordenesDescartadas = 0;

    for (int hora = 0; hora < 24; ++hora) {
      std::cout << " \n ========== Tick " << hora << " ========== \n" << std::endl;
      grid.setTickActual(hora);

      std::string sufijo = (hora < 10) ? "0" : "";
      const std::string horaStr = sufijo + std::to_string(hora);
      const std::string ruta = cfg.datosDir + "/ofertas_" + horaStr + ".csv";
      const std::string tickHora = timestampTick(cfg, hora);

      if (tickHora.empty()) {
        std::cerr << "Advertencia: 'fecha_simulada' inválida en config.ini ("
                  << cfg.fechaSimulada
                  << "); se usará un timestamp de respaldo.\n";
      }

      // Estado del dominio al inicio del tick. El matching ya mueve saldos y
      // la batería en memoria antes de persistir: si la base de datos rechaza
      // el tick hay que devolver todo a este punto, o la simulación sigue con
      // una energía que nunca se vendió.
      std::map<int, double> saldosPrevio;
      for (const auto &[id, nodo] : nodos)
        saldosPrevio[id] = nodo->getSaldoCuenta();
      const double cargaPrevia = bateria ? bateria->getCargaActual() : 0.0;

      if (bateria) {
        std::cout << "[Tick " << horaStr << "] Batería inicio: nodo "
                  << bateria->getId() << ", " << bateria->estado() << std::endl;
      }

      debugPrint("[Tick " + horaStr + "] Leyendo " + ruta);

      // Un CSV ausente o con filas inválidas no debe matar la simulación:
      // se informa y el tick sigue con las órdenes que sí se pudieron leer.
      const auto informe = CapaDatos::leerCSVConInforme(ruta);
      for (const auto &motivo : informe.rechazadas) {
        ++totalRechazados;
        std::cout << "[CSV] " << ruta << " - " << motivo << std::endl;
      }

      int compras = 0, ventas = 0;
      for (const auto &o : informe.filas) {
        if (o.esCompra)
          ++compras;
        else
          ++ventas;
      }
      debugPrint("[Tick " + horaStr + "] Órdenes cargadas: " +
                 std::to_string(informe.filas.size()) + " (" +
                 std::to_string(compras) + " compra / " +
                 std::to_string(ventas) + " venta)");

      const double tarifa = gestor.leerTarifa(hora);
      if (bateria) {
        debugPrint("[Tick " + horaStr + "] Tarifa hora = " +
                   std::to_string(tarifa) + " | Batería nodo " +
                   std::to_string(bateria->getId()) + " carga = " +
                   std::to_string(bateria->getCargaActual()) + " kWh");
      }

      // La orquestración del tick (oferta de batería -> CSV -> matching ->
      // excedentes -> reintentos) vive en GridManager::procesarTick.
      const std::vector<TransaccionEnergia> txns =
          grid.procesarTick(informe.filas, bateria, tarifa);
      ordenesDescartadas += grid.ordenesDescartadas();

      double kwhTick = 0.0;
      for (const auto &t : txns)
        kwhTick += t.kwh;
      debugPrint("[Tick " + horaStr + "] Matching: " +
                 std::to_string(txns.size()) + " transacciones (" +
                 std::to_string(kwhTick) + " kWh)");

      std::map<int, double> saldosActuales;
      for (const auto &[id, nodo] : nodos)
        saldosActuales[id] = nodo->getSaldoCuenta();

      // Persistencia transaccional del tick (bloque atómico): transacciones,
      // saldos y lecturas se confirman juntos o no se confirma nada.
      const ResultadoTick r = gestor.persistirTick(txns, saldosActuales, tickHora);

      if (r.ok) {
        totalTransacciones += r.transacciones;
        totalKwh += kwhTick;
        totalLecturas += r.lecturas;
        debugPrint("[Tick " + horaStr + "] COMMIT OK: " +
                   std::to_string(r.transacciones) + " transacciones y " +
                   std::to_string(r.lecturas) + " lecturas persistidas");

        // La batería se descarga recién con el tick confirmado: antes era una
        // consecuencia en memoria del matching, sin rollback posible.
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
        for (const auto &[id, nodo] : nodos) {
          auto it = saldosPrevio.find(id);
          if (it != saldosPrevio.end())
            nodo->setSaldoCuenta(it->second);
        }
        if (bateria)
          bateria->restablecerCarga(cargaPrevia);
        logger("[Tick " + horaStr +
               "] Tick rechazado: ROLLBACK aplicado, saldos y carga de "
               "batería restaurados.");
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
    std::cout << "Órdenes descartadas por datos inválidos: " << ordenesDescartadas
              << "\n";
    std::cout << "Filas de CSV rechazadas: " << totalRechazados << "\n";
    std::cout << "Simulación finalizada.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Error fatal: " << e.what() << "\n";
    return 1;
  }
}
