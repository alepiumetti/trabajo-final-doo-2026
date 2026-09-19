#ifndef types_h
#define types_h
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

    void log(const std::string& msg) {
        if (logger) logger(msg);
    }

public:
    GridManager() = default;

    explicit GridManager(std::function<void(const std::string&)> logFn)
        : logger(std::move(logFn)) {}

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

        while (!bidMap.empty() && !askMap.empty()) {
            auto mejorBid = bidMap.begin(); // mayor precio de compra
            auto mejorAsk = askMap.begin(); // menor precio de venta

            // Si los precios NO son compatibles, fin del matching
            if (mejorBid->first < mejorAsk->first) {
                break;
            }

            // Copias locales de las órdenes al frente de cada cola
            Orden ordenCompra = mejorBid->second.front();
            Orden ordenVenta  = mejorAsk->second.front();

            double energia = std::min(ordenCompra.kwh, ordenVenta.kwh);
            double precio  = (ordenCompra.precio + ordenVenta.precio) / 2.0;

            // Registrar transacción en memoria
            registrarTransaccion(ordenVenta.idNodo, ordenCompra.idNodo,
                                 energia, precio);

            // Actualizar remanentes
            ordenCompra.kwh -= energia;
            ordenVenta.kwh  -= energia;

            // Reencolar o eliminar según remanente
            actualizarCola(mejorBid, ordenCompra);
            actualizarCola(mejorAsk, ordenVenta);

            // Limpiar entradas del mapa si la cola quedó vacía
            if (mejorBid->second.empty()) bidMap.erase(mejorBid);
            if (mejorAsk->second.empty()) askMap.erase(mejorAsk);
        }

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

                double absorbido = bateria.absorberEnergia(orden.kwh);
                if (absorbido <= UMBRAL) {
                    log("[Bateria] Capacidad llena, no puede absorber más.");
                    return;
                }

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

private:
    // ----------------------------------------------------------
    // Registra una transacción en el vector del tick
    // ----------------------------------------------------------
    void registrarTransaccion(int idVendedor, int idComprador,
                              double kwh, double precio) {
        transaccionesDelTick.emplace_back(
            idVendedor, idComprador, kwh, precio, tickActual);

        log("[Transaccion] Vendedor=" + std::to_string(idVendedor) +
            " Comprador=" + std::to_string(idComprador) +
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
