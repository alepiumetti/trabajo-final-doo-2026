#ifndef types_h
#define types_h

#define BATERIA 5 // id del nodo Bateria (seed en sql/crear_esquema.sql)

#include <algorithm>
#include <chrono>
#include <functional>
#include <queue>
#include <utility>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
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

};

//  ========================== TRANSACCION ENERGIA  ==========================

struct TransaccionEnergia {
    int idVendedor;
    int idComprador;
    double kwh;
    double precio;
    std::chrono::system_clock::time_point timestamp;
    int tickHora; // hora simulada (0-23) para trazabilidad

    TransaccionEnergia() = default;

    TransaccionEnergia(int idVendedor, int idComprador,
                       double kwh, double precio, int tickHora)
        : idVendedor(idVendedor), idComprador(idComprador),
          kwh(kwh), precio(precio),
          timestamp(std::chrono::system_clock::now()),
          tickHora(tickHora) {}

    double getMontoTotal() const { return kwh * precio; }
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

    // Órdenes de compra sin saldo suficiente (se reintentan dentro del tick)
    std::vector<Orden> pendientesPorSaldo;

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

    void setDebug(bool d) { debug_ = d; }

    // ----------------------------------------------------------
    // Inserción de órdenes en el libro
    // ----------------------------------------------------------
    void insertarOrden(const Orden& ordenOriginal) {
        Orden orden = ordenOriginal;
        orden.secuencia = ++contadorSecuencia;

        if (orden.esCompra) {
            bidMap[orden.precio].push(orden);
        } else {
            askMap[orden.precio].push(orden);
        }
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
                    log("[Bateria] Saldo insuficiente para absorber " +
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

    void setTickActual(int hora) { tickActual = hora; }

    bool libroVacio() const { return bidMap.empty() && askMap.empty(); }

    const std::vector<TransaccionEnergia>& getTransacciones() const {
        return transaccionesDelTick;
    }

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
                log("[Saldo insuficiente] Nodo " +
                    std::to_string(ordenCompra.idNodo) +
                    " no puede pagar " + std::to_string(monto) +
                    " (kWh=" + std::to_string(energia) +
                    " a $" + std::to_string(precio) + ")");
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
