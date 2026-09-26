#ifndef types_h
#define types_h

// Id del nodo Batería Comunitaria. Debe coincidir con la semilla de
// sql/crear_esquema.sql y con los casos de aceptación del PDF.
#define BATERIA 99

#include <algorithm>
#include <chrono>
#include <functional>
#include <queue>
#include <utility>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

enum PerfilConsumo { Residencial, Comercial, Industrial };

struct Orden {
  int idOrden;
  bool esCompra; // true=compra, false=venta
  int idNodo;
  double precio;
  double kwh;
  uint64_t secuencia; // timestamp o contador FIFO

  Orden() = default;

  Orden(int idOrden, bool esCompra, int idNodo,
          double precio, double kwh, uint64_t secuencia = 0)
        : idOrden(idOrden), esCompra(esCompra), idNodo(idNodo),
          precio(precio), kwh(kwh), secuencia(secuencia) {}

};


//  ========================== NODO CONSUMIDOR  ==========================

class NodoRed {
protected:
  int id;
  std::string ubicacion;
  double balanceEnergia;
  double saldoCuenta;

public:
    NodoRed(int id, std::string ubicacion,
        double balanceEnergia = 0.0, double saldoCuenta = 0.0)
        : id(id), ubicacion(std::move(ubicacion)),
        balanceEnergia(balanceEnergia), saldoCuenta(saldoCuenta) {}

  virtual double calcularExcedente() const = 0;

  int getId() const { return id;}

  double getSaldoCuenta() const { return saldoCuenta; }
  void setSaldoCuenta(double nuevoSaldo) { saldoCuenta = nuevoSaldo; }

  virtual ~NodoRed() = default;

};


//  ========================== NODO CONSUMIDOR  ==========================

class NodoConsumidor : public NodoRed {

private:
    PerfilConsumo perfil;

public:
    NodoConsumidor(int id, std::string ubicacion, PerfilConsumo perfil,
        double balanceEnergia = 0.0, double saldoCuenta = 0.0)
        : NodoRed(id, std::move(ubicacion), balanceEnergia, saldoCuenta),
          perfil(perfil) {}


  double calcularExcedente() const override {

        if(balanceEnergia > 0.0) {
            return 0.0;
        }

        return balanceEnergia;
  };
};


//  ========================== NODO PROSUMIDOR  ==========================

class NodoProsumidor : public NodoRed {
private:
  double produccion;
  double consumo;

public:
        NodoProsumidor(int id, std::string ubicacion,
                   double produccion = 0.0, double consumo = 0.0,
                   double saldoCuenta = 0.0)
        : NodoRed(id, std::move(ubicacion), produccion - consumo, saldoCuenta),
          produccion(produccion), consumo(consumo) {}

  double calcularExcedente() const override{
    return produccion - consumo;
  };
};


//  ========================== NODO ALMACENAMIENTO  ==========================

class NodoAlmacenamiento : public NodoRed {
private:
  double cargaActual;
  double capacidadMax;

public:
    NodoAlmacenamiento(int id, std::string ubicacion,
                       double cargaActual = 0.0,
                       double capacidadMax = 1000.0,
                       double saldoCuenta = 0.0)
        : NodoRed(id, std::move(ubicacion), cargaActual, saldoCuenta),
          cargaActual(cargaActual), capacidadMax(capacidadMax) {}

    double calcularExcedente() const override{
        return cargaActual;
    };

    double getCargaActual() const { return cargaActual; }

    double capacidadDisponible() const { return capacidadMax - cargaActual; }

    std::string estado() const {
        return "carga = " + std::to_string(cargaActual) +
               " kWh, capacidad = " + std::to_string(capacidadMax) +
               " kWh, disponible = " + std::to_string(capacidadMax - cargaActual) +
               " kWh, saldo = " + std::to_string(saldoCuenta) + " créditos";
    }

    double absorberEnergia(double kwh) {
        double espacio = capacidadMax - cargaActual;
        double absorbido = std::min(kwh, espacio);
        cargaActual += absorbido;
        balanceEnergia = cargaActual;
        return absorbido;
    }

    double liberarEnergia(double kwh) {
        double liberado = std::min(kwh, cargaActual);
        cargaActual -= liberado;
        balanceEnergia = cargaActual;
        return liberado;
    }

    // Devuelve la carga a un valor previo: se usa para revertir un tick
    // que la base de datos rechazó (la energía absorbida durante el
    // matching no se puede deshacer transacción por transacción).
    void restablecerCarga(double kwh) {
        cargaActual = std::clamp(kwh, 0.0, capacidadMax);
        balanceEnergia = cargaActual;
    }

};

//  ========================== TRANSACCION ENERGIA  ==========================

struct TransaccionEnergia {
    int idVendedor;
    int idComprador;
    double kwh;
    double precio;
    std::chrono::system_clock::time_point timestamp;
    int tickHora; // hora simulada (0-23) para trazabilidad

    TransaccionEnergia(int idVendedor, int idComprador,
                       double kwh, double precio, int tickHora)
        : idVendedor(idVendedor), idComprador(idComprador),
          kwh(kwh), precio(precio),
          timestamp(std::chrono::system_clock::now()),
          tickHora(tickHora) {}
};

//  ========================== NODO CONSUMIDOR  ==========================

class GridManager {
public:
    // Mapa de compras: mayor precio primero (std::greater)
    using BidMap = std::map<double, std::queue<Orden>, std::greater<double>>;
    // Mapa de ventas: menor precio primero (orden por defecto)
    using AskMap = std::map<double, std::queue<Orden>>;

private:
    BidMap bidMap;
    AskMap askMap;

    // Contador FIFO para desempate dentro del mismo precio
    uint64_t contadorSecuencia = 0;

    // Transacciones generadas en el tick actual (a persistir)
    std::vector<TransaccionEnergia> transaccionesDelTick;

    // Umbral de energía residual para considerar una orden "completada"
    static constexpr double UMBRAL = 0.001;

    int tickActual = 0;

    // Callback para logging (desacopla de std::cout)
    std::function<void(const std::string&)> logger;

    // Flag de depuración: con --debug se imprime el detalle y se pausa
    // tras cada log (modo paso a paso).
    bool debug_ = false;

    // Callbacks para consultar/actualizar saldo (desacoplan la persistencia)
    std::function<double(int)> consultarSaldo;
    std::function<bool(int, double)> actualizarSaldo;

    // Indica si un id de nodo existe en la BD. Opcional: si no se
    // define, la validación de nodos inexistentes queda desactivada.
    std::function<bool(int)> nodoConocido;

    // Órdenes de compra sin saldo suficiente (se reintentan dentro del tick)
    std::vector<Orden> pendientesPorSaldo;

    // Cuántas órdenes descartó insertarOrden en el tick actual por datos
    // inválidos (precio no positivo, energía no positiva o nodo
    // inexistente). procesarTick lo pone en cero en cada tick.
    size_t ordenesDescartadas_ = 0;

    // En modo debug y con stdin como terminal, espera Enter para continuar.
    void pausa() {
        if (debug_ && isatty(STDIN_FILENO)) {
            std::cout << "  (Enter para continuar)\n";
            std::cin.get();
        }
    }

    void log(const std::string& msg) {
        if (logger) logger(msg);
        pausa();
    }

    // Solo imprime (y pausa) si --debug está activo.
    void debugLog(const std::string& msg) {
        if (!debug_) return;
        if (logger) logger(msg);
        pausa();
    }

public:
    GridManager() = default;

    explicit GridManager(std::function<void(const std::string&)> logFn)
        : logger(std::move(logFn)) {}

    GridManager(std::function<double(int)> consultarSaldoFn,
                std::function<bool(int, double)> actualizarSaldoFn,
                std::function<void(const std::string&)> logFn)
        : logger(std::move(logFn)),
          consultarSaldo(std::move(consultarSaldoFn)),
          actualizarSaldo(std::move(actualizarSaldoFn)) {}

  GridManager(std::function<double(int)> consultarSaldoFn,
                std::function<bool(int, double)> actualizarSaldoFn,
                std::function<void(const std::string&)> logFn,
                std::function<bool(int)> nodoConocidoFn)
        : logger(std::move(logFn)),
          consultarSaldo(std::move(consultarSaldoFn)),
          actualizarSaldo(std::move(actualizarSaldoFn)),
          nodoConocido(std::move(nodoConocidoFn)) {}

    void setDebug(bool d) { debug_ = d; }

    // ----------------------------------------------------------
    // Inserción de órdenes en el libro
    // ----------------------------------------------------------
    // Valida la orden ANTES de meterla en el libro. Sin esto, una fila
    // con kwh = 0 llegaría al matching, generaría una transacción de
    // 0 kWh y el CHECK (kwh > 0) de la BD abortaría el tick ENTERO,
    // perdiendo también las transacciones legítimas del tick.
    // Devuelve false si la orden se descartó.
    bool insertarOrden(const Orden& ordenOriginal) {
        // El PDF (sección 3.3): los precios no pueden ser negativos.
        if (ordenOriginal.precio <= 0.0) {
            log("[Orden descartada] Nodo " +
                std::to_string(ordenOriginal.idNodo) + ": precio " +
                std::to_string(ordenOriginal.precio) +
                " invalido (debe ser > 0).");
            ++ordenesDescartadas_;
            return false;
        }
        if (ordenOriginal.kwh <= UMBRAL) {
            log("[Orden descartada] Nodo " +
                std::to_string(ordenOriginal.idNodo) + ": energia " +
                std::to_string(ordenOriginal.kwh) +
                " kWh invalida (debe ser > 0).");
            ++ordenesDescartadas_;
            return false;
        }
        // El PDF (sección 6.1): los nodos deben existir en la BD. Sin
        // esta comprobación, un id inexistente entra al libro y el
        // matching lo descarta como "compra sin saldo", que es un
        // diagnóstico falso.
        if (nodoConocido && !nodoConocido(ordenOriginal.idNodo)) {
            log("[Orden descartada] El nodo " +
                std::to_string(ordenOriginal.idNodo) +
                " no existe en la base de datos.");
            ++ordenesDescartadas_;
            return false;
        }

        Orden orden = ordenOriginal;
        orden.secuencia = ++contadorSecuencia;

        if (orden.esCompra) {
            bidMap[orden.precio].push(orden);
        } else {
            askMap[orden.precio].push(orden);
        }
        return true;
    }

    // Inserta la oferta de la batería al inicio del libro de ventas
    // (se llama antes de procesar el CSV del tick).
    void insertarOfertaBateria(int idBateria, double kwh, double precioBase) {
        if (kwh <= UMBRAL) return;

        Orden ordenBateria(
            /*idOrden*/ 0,          // 0 = orden especial
            false,
            idBateria,
            precioBase,
            kwh
        );
        insertarOrden(ordenBateria);

        log("[Bateria] Oferta insertada: " + std::to_string(kwh) +
            " kWh a " + std::to_string(precioBase));
    }

    // ----------------------------------------------------------
    // Algoritmo de Matching (Sección 5.2 del PDF)
    // ----------------------------------------------------------
    std::vector<TransaccionEnergia> ejecutarMatching() {
        transaccionesDelTick.clear();
        realizarMatching();
        return transaccionesDelTick;
    }

    // ----------------------------------------------------------
    // Transferencia de excedentes a la batería
    // (Sección 3.2 y 3.3 del PDF)
    // ----------------------------------------------------------
    void transferirExcedentesABateria(NodoAlmacenamiento& bateria,
                                      double precioBaseHora) {
        // Recorremos todas las órdenes de venta remanentes
        for (auto& [precio, cola] : askMap) {
            while (!cola.empty()) {
                Orden orden = cola.front();

                if (orden.kwh <= UMBRAL) {
                    cola.pop();
                    continue;
                }

                // La oferta propia de la batería es energía ya almacenada:
                // no debe "absorberse" a sí misma si no se vendió.
                if (orden.idNodo == bateria.getId()) {
                    cola.pop();
                    continue;
                }

                double absorbible = std::min(orden.kwh, bateria.capacidadDisponible());
                if (absorbible <= UMBRAL) {
                    log("[Bateria] Capacidad llena, no puede absorber más.");
                    return;
                }

                double monto = absorbible * precioBaseHora;
                if (consultarSaldo && actualizarSaldo &&
                    consultarSaldo(bateria.getId()) < monto) {
                    log("[Bateria] Saldo insuficiente en memoria para absorber " +
                        std::to_string(absorbible) + " kWh.");
                    return;
                }

                double absorbido = bateria.absorberEnergia(orden.kwh);

                // Transacción batería <- vendedor
                registrarTransaccion(orden.idNodo, bateria.getId(),
                                     absorbido, precioBaseHora);

                orden.kwh -= absorbido;

                if (orden.kwh <= UMBRAL) {
                    cola.pop();
                } else {
                    cola.pop();
                    cola.push(orden);
                }
            }
        }

        // Eliminar precios cuyas colas quedaron vacías
        for (auto it = askMap.begin(); it != askMap.end(); ) {
            if (it->second.empty()) it = askMap.erase(it);
            else ++it;
        }
    }

    // ----------------------------------------------------------
    // Elimina remanentes al final del tick
    // ----------------------------------------------------------
    // Órdenes descartadas por validación en el ÚLTIMO tick. La usa
    // main.cpp para acumular el total del día.
    size_t ordenesDescartadas() const { return ordenesDescartadas_; }

    void limpiarLibroAlFinalDelTick() {
    // Demanda insatisfecha: log
    registrarDemandaInsatisfecha();

    // Excedente no absorbido por batería: log y descartar
    for (auto& [precio, cola] : askMap) {
        while (!cola.empty()) {
            const Orden& orden = cola.front();
            log("[Excedente no absorbido] Nodo " +
                std::to_string(orden.idNodo) +
                " quedó con " + std::to_string(orden.kwh) +
                " kWh a " + std::to_string(orden.precio));
            cola.pop();
        }
    }
    limpiarLibro();
}
    // ----------------------------------------------------------
    // Registrar demanda insatisfecha (compras sin match)
    // ----------------------------------------------------------
    void registrarDemandaInsatisfecha() {
        for (auto& [precio, cola] : bidMap) {
            while (!cola.empty()) {
                const Orden& orden = cola.front();
                log("[Demanda insatisfecha] Nodo " +
                    std::to_string(orden.idNodo) +
                    " no pudo comprar " + std::to_string(orden.kwh) +
                    " kWh a " + std::to_string(orden.precio));
                cola.pop();
            }
        }
        bidMap.clear();

        for (const Orden& orden : pendientesPorSaldo) {
            log("[Demanda insatisfecha por saldo] Nodo " +
                std::to_string(orden.idNodo) +
                " no pudo comprar " + std::to_string(orden.kwh) +
                " kWh a " + std::to_string(orden.precio));
        }
        pendientesPorSaldo.clear();
    }

    // ----------------------------------------------------------
    // Utilidades
    // ----------------------------------------------------------
    void limpiarLibro() {
        bidMap.clear();
        askMap.clear();
    }

    // ----------------------------------------------------------
    // procesarTick: método principal del motor de subasta.
    //
    // El PDF (sección 4.1) pide que GridManager tenga
    // `procesarTick(const std::vector& ofertasCSV)`. Este overload
    // concentra la orquestración del tick —que antes vivía entera en
    // main.cpp— para que el motor sea usable y testeable por separado.
    //
    // Orden de las operaciones (secciones 3.2 y 3.3 del PDF):
    //   1. La batería ofrece su carga al precio base, ANTES del CSV.
    //      La sección 3.3 dice textualmente que su oferta "se inserta
    //      al inicio del libro de ventas antes de procesar las del
    //      CSV", y el caso 4 depende de ese orden: es lo que da
    //      prioridad precio-tiempo a la energía almacenada.
    //   2. Se cargan las órdenes del CSV.
    //   3. Matching.
    //   4. Los excedentes sin comprador van a la batería.
    //   5. Se reintentan las compras que quedaron sin saldo.
    //
    // Devuelve las transacciones del tick. El llamador las persiste en
    // un único bloque transaccional (CapaDatos::persistirTick) y recién
    // después descarga la batería: si la BD rechaza el tick, la energía
    // nunca se considers vendida.
    std::vector<TransaccionEnergia>
    procesarTick(const std::vector<Orden> &ofertasCSV,
                 NodoAlmacenamiento *bateria, double precioBaseHora) {
        // El contador es POR TICK: el llamador lo acumula para el resumen
        // del día. Si fuera acumulado, cada tick sumaria también los
        // descartes de los ticks anteriores.
        ordenesDescartadas_ = 0;

        if (bateria) {
            insertarOfertaBateria(bateria->getId(), bateria->getCargaActual(),
                                  precioBaseHora);
        }

        for (const auto &o : ofertasCSV) {
            insertarOrden(o);
        }

        ejecutarMatching();

        if (bateria) {
            transferirExcedentesABateria(*bateria, precioBaseHora);
        }

        reevaluarPendientesPorSaldo();

        return transaccionesDelTick;
    }

    void setTickActual(int hora) { tickActual = hora; }

    // Reintenta las órdenes de compra que quedaron sin saldo (dentro del tick)
    void reevaluarPendientesPorSaldo() {
        if (pendientesPorSaldo.empty()) return;

        for (const Orden& orden : pendientesPorSaldo) {
            Orden reinsertada = orden;
            reinsertada.secuencia = ++contadorSecuencia;
            bidMap[reinsertada.precio].push(reinsertada);
        }
        pendientesPorSaldo.clear();

        realizarMatching();
    }

private:
    // ----------------------------------------------------------
    // Algoritmo de Matching (Sección 5.2 del PDF)
    // ----------------------------------------------------------
    void realizarMatching() {
        while (!bidMap.empty() && !askMap.empty()) {
            auto mejorBid = bidMap.begin(); // mayor precio de compra
            auto mejorAsk = askMap.begin(); // menor precio de venta

            // Copias locales de las órdenes al frente de cada cola
            Orden ordenCompra = mejorBid->second.front();
            Orden ordenVenta  = mejorAsk->second.front();

            debugLog("[Matching] mejorBid=" + std::to_string(mejorBid->first) +
                     " (nodo " + std::to_string(ordenCompra.idNodo) + ", " +
                     std::to_string(ordenCompra.kwh) + " kWh) | mejorAsk=" +
                     std::to_string(mejorAsk->first) + " (nodo " +
                     std::to_string(ordenVenta.idNodo) + ", " +
                     std::to_string(ordenVenta.kwh) + " kWh)");

            // Si los precios NO son compatibles, fin del matching
            if (mejorBid->first < mejorAsk->first) {
                debugLog("[Matching] Sin cruce compatible: " +
                         std::to_string(mejorBid->first) + " < " +
                         std::to_string(mejorAsk->first) + " -> fin del matching");
                break;
            }

            double energia = std::min(ordenCompra.kwh, ordenVenta.kwh);
            double precio  = (ordenCompra.precio + ordenVenta.precio) / 2.0;
            double monto   = energia * precio;

            debugLog("[Matching] Cruce: energia=" + std::to_string(energia) +
                     " kWh, precio_clearing=" + std::to_string(precio));

            // Si el comprador no tiene saldo, la orden queda pendiente
            // para reintentarla dentro del tick (tras las transferencias).
            //
            // Ojo: esto es el motor EN MEMORIA, no el trigger de SQLite
            // (trg_validar_saldo). El trigger real no llega a dispararse con
            // estos datos: ambos miran el mismo saldo y aplican el mismo
            // predicado, así que si acá no hay saldo la compra ni se
            // intenta. El log lo dice explícito para no confundirlos.
            if (consultarSaldo && actualizarSaldo &&
                consultarSaldo(ordenCompra.idNodo) < monto) {
                debugLog("[Matching] Comprador nodo " +
                         std::to_string(ordenCompra.idNodo) +
                         " sin saldo (" + std::to_string(monto) +
                         " > " +
                         std::to_string(consultarSaldo(ordenCompra.idNodo)) +
                         "): orden a reintento");
                mejorBid->second.pop();
                if (mejorBid->second.empty()) bidMap.erase(mejorBid);
                pendientesPorSaldo.push_back(ordenCompra);
                log("[Saldo insuficiente en memoria] Nodo " +
                    std::to_string(ordenCompra.idNodo) +
                    " no puede pagar " + std::to_string(monto) +
                    " (kWh=" + std::to_string(energia) +
                    " a $" + std::to_string(precio) +
                    ")");
                continue;
            }

            // Registrar transacción en memoria
            registrarTransaccion(ordenVenta.idNodo, ordenCompra.idNodo,
                                 energia, precio);

            // Actualizar remanentes
            ordenCompra.kwh -= energia;
            ordenVenta.kwh  -= energia;

            // Reencolar o eliminar según remanente
            actualizarCola(mejorBid, ordenCompra);
            actualizarCola(mejorAsk, ordenVenta);

            if (ordenCompra.kwh > UMBRAL)
                debugLog("[Matching] Compra nodo " +
                         std::to_string(ordenCompra.idNodo) + " remanente " +
                         std::to_string(ordenCompra.kwh) + " kWh (reinsertada)");
            else
                debugLog("[Matching] Compra nodo " +
                         std::to_string(ordenCompra.idNodo) + " completada");

            if (ordenVenta.kwh > UMBRAL)
                debugLog("[Matching] Venta nodo " +
                         std::to_string(ordenVenta.idNodo) + " remanente " +
                         std::to_string(ordenVenta.kwh) + " kWh (reinsertada)");
            else
                debugLog("[Matching] Venta nodo " +
                         std::to_string(ordenVenta.idNodo) + " completada");

            // Limpiar entradas del mapa si la cola quedó vacía
            if (mejorBid->second.empty()) bidMap.erase(mejorBid);
            if (mejorAsk->second.empty()) askMap.erase(mejorAsk);
        }
    }

    // ----------------------------------------------------------
    // Registra una transacción en el vector del tick
    // ----------------------------------------------------------
    void registrarTransaccion(int idVendedor, int idComprador,
                              double kwh, double precio) {
        // Un nodo no puede comprarse a sí mismo: el crédito y el débito caen
        // sobre el mismo saldo y el asiento queda mal (en memoria se anulan,
        // en el ledger queda el movimiento completo).
        if (idVendedor == idComprador) {
            log("[Transaccion] Descartada: el nodo " +
                std::to_string(idVendedor) +
                " no puede ser vendedor y comprador en la misma operacion.");
            return;
        }

        transaccionesDelTick.emplace_back(
            idVendedor, idComprador, kwh, precio, tickActual);

        // Debitar al comprador y acreditar al vendedor (saldo en dominio)
        if (consultarSaldo && actualizarSaldo) {
            double monto = kwh * precio;
            double saldoVendedor = consultarSaldo(idVendedor);
            double saldoComprador = consultarSaldo(idComprador);
            actualizarSaldo(idVendedor, saldoVendedor + monto);
            actualizarSaldo(idComprador, saldoComprador - monto);
        }

        log("[Transaccion] Vendedor=" +
            (idVendedor == BATERIA ? std::string("Bateria")
                                   : std::to_string(idVendedor)) +
            " Comprador=" +
            (idComprador == BATERIA ? std::string("Bateria")
                                    : std::to_string(idComprador)) +
            " kWh=" + std::to_string(kwh) +
            " Precio=" + std::to_string(precio));
    }

    // ----------------------------------------------------------
    // Actualiza la cola del mapa según el remanente de la orden
    // ----------------------------------------------------------
    template <typename MapIterator>
    void actualizarCola(MapIterator it, const Orden& orden) {
        it->second.pop(); // siempre sacamos el frente
        if (orden.kwh > UMBRAL) {
            it->second.push(orden); // reencolamos remanente al final
        }
    }
};

#endif // types_h
